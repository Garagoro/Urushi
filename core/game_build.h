#pragma once

#include <cstdint>

// Detects which Steam branch of the game is running (release vs alpha-test) by
// fingerprinting GameAssembly.dll's PE header. Signatures are always scanned first;
// the per-branch RVAs below are only fallbacks when a signature misses.
//
// Unknown builds (a future patch of either branch) get NO fallback RVAs: calling a stale
// address in an unknown binary would crash, so an unmatched signature just disables
// that feature instead.
enum class GameBranch : uint8_t {
    Unknown = 0,
    Release,
    Alpha,
};

struct BranchRva {
    uintptr_t release;
    uintptr_t alpha;
};

namespace GameBuild {

// Fingerprint GameAssembly.dll (cached after the first successful call).
GameBranch Get();
const char* Name(GameBranch b);
inline const char* Name() { return Name(Get()); }

uint32_t TimeDateStamp();
uint32_t SizeOfImage();

// Fallback RVA for the running branch, 0 on Unknown.
uintptr_t Pick(const BranchRva& rva);

} // namespace GameBuild
