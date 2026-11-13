#include "game_build.h"
#include "logger.h"

#include <windows.h>
#include <atomic>
#include <cstdio>

namespace {

struct KnownBuild {
    GameBranch branch;
    uint32_t timeDateStamp;
    uint32_t sizeOfImage;
};

// GameAssembly.dll PE fingerprints. Add a row here when a branch updates and its RVAs are re-verified.
constexpr KnownBuild kKnownBuilds[] = {
    { GameBranch::Release, 0x6AEDB2D6u, 0x09282000u }, // release (Nov 2026)
    { GameBranch::Alpha,   0x6AEDE413u, 0x0A7AE000u }, // alpha-test, Steam build 26487066
};

std::atomic<bool> s_resolved{ false };
GameBranch s_branch = GameBranch::Unknown;
uint32_t s_ts = 0;
uint32_t s_soi = 0;

bool ReadPeFingerprint(HMODULE mod, uint32_t& ts, uint32_t& soi) {
    __try {
        auto base = reinterpret_cast<const uint8_t*>(mod);
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        ts = nt->FileHeader.TimeDateStamp;
        soi = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

namespace GameBuild {

GameBranch Get() {
    if (s_resolved.load(std::memory_order_acquire)) return s_branch;

    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    uint32_t ts = 0, soi = 0;
    if (!ga || !ReadPeFingerprint(ga, ts, soi)) return GameBranch::Unknown; // not loaded yet: retry later

    GameBranch b = GameBranch::Unknown;
    for (const auto& k : kKnownBuilds) {
        if (k.timeDateStamp == ts && k.sizeOfImage == soi) { b = k.branch; break; }
    }

    // Benign race: concurrent first callers compute the same value.
    s_ts = ts;
    s_soi = soi;
    s_branch = b;
    s_resolved.store(true, std::memory_order_release);

    char buf[160];
    std::snprintf(buf, sizeof(buf), "GameBuild: GameAssembly.dll ts=0x%08X size=0x%08X -> %s%s",
                  ts, soi, Name(b),
                  b == GameBranch::Unknown ? " (signature scan only, no fallback RVAs)" : "");
    if (b == GameBranch::Unknown) Logger::Warn(buf); else Logger::Info(buf);
    return b;
}

const char* Name(GameBranch b) {
    switch (b) {
        case GameBranch::Release: return "release";
        case GameBranch::Alpha:   return "alpha";
        default:                  return "unknown";
    }
}

uint32_t TimeDateStamp() { Get(); return s_ts; }
uint32_t SizeOfImage() { Get(); return s_soi; }

uintptr_t Pick(const BranchRva& rva) {
    switch (Get()) {
        case GameBranch::Release: return rva.release;
        case GameBranch::Alpha:   return rva.alpha;
        default:                  return 0;
    }
}

} // namespace GameBuild
