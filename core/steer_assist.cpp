#include "steer_assist.h"
#include "pattern_scanner.h"
#include "native_carx.h"
#include "logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

// Dynamic RVAs inside NativeCarX.dll (default fallback RVAs for vanilla build)
static uintptr_t s_rvaCenterScaleMul    = 0x90DE; // F3 0F 59 05 [disp32]  mulss xmm0, [disp32] (0.3f)
static uintptr_t s_rvaMode0SlipJump     = 0x91F9; // 76 40                  jbe 0x923B
static uintptr_t s_rvaSlipGatePosJump   = 0x92C8; // 76 1B                  jbe 0x92E5 (-1.3f gate)
static uintptr_t s_rvaSlipGateNegJump   = 0x92E9; // 73 1B                  jae 0x9306 (+1.3f gate)
static uintptr_t s_rvaSteerFlipComiss   = 0xA5C5; // 0F 2F 0D [disp32]      comiss xmm1, [disp32] (90 deg)
static uintptr_t s_rvaUncondFlipComiss  = 0xA5F3; // 0F 2F 0D [disp32]      comiss xmm1, [disp32] (140 deg)

// Verified Car / Wheel struct offsets for live telemetry readout
constexpr uintptr_t kOffSteerMode         = 0x4E8;
constexpr uintptr_t kOffSteerFFMode       = 0x5BC;
constexpr uintptr_t kOffAssistActive      = 0x5F0;
constexpr uintptr_t kOffSpeedMps          = 0x610;
constexpr uintptr_t kOffMaxSteerRad       = 0x838;
constexpr uintptr_t kOffFFBTorque         = 0xB0C;
constexpr uintptr_t kOffWheelFLIndex      = 0xB3C;
constexpr uintptr_t kOffWheelFRIndex      = 0xB40;
constexpr uintptr_t kOffWheelsBase        = 0xC18;
constexpr uintptr_t kWheelStride          = 0x26C80;
constexpr uintptr_t kOffWheelSlipAngleNeg = 0xF6C;
constexpr uintptr_t kOffWheelHasSlipGate  = 0x26930;

static std::array<uint8_t, 8> s_origCenterScale = { 0xF3, 0x0F, 0x59, 0x05, 0x2A, 0xD2, 0x08, 0x00 };
static std::array<uint8_t, 2> s_origMode0Jump   = { 0x76, 0x40 };
static std::array<uint8_t, 2> s_origPosGateJump = { 0x76, 0x1B };
static std::array<uint8_t, 2> s_origNegGateJump = { 0x73, 0x1B };
static std::array<uint8_t, 7> s_origSteerFlip   = { 0x0F, 0x2F, 0x0D, 0x54, 0xBE, 0x08, 0x00 };
static std::array<uint8_t, 7> s_origUncondFlip  = { 0x0F, 0x2F, 0x0D, 0x8E, 0xBE, 0x08, 0x00 };

struct NearFloatBlock {
    volatile float uncondFlipRad;
    volatile float steerFlipRad;
    volatile float centerScale;
};

std::recursive_mutex s_mutex;
SteerAssist::Config s_cfg{};
bool s_initialized = false;
bool s_nativeFound = false;
bool s_bytesVerified = false;
bool s_nearAllocOk = false;
bool s_flipPatched = false;
bool s_slipPatched = false;
uintptr_t s_nativeBase = 0;
NearFloatBlock* s_nearBlock = nullptr;

template <typename T>
static bool SafeReadValue(uintptr_t addr, T& outVal) {
    __try {
        if (addr < 0x10000) return false;
        outVal = *reinterpret_cast<const T*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteCodeBytes(uintptr_t addr, const uint8_t* data, size_t len) {
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(addr), data, len);
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), len, oldProt, &unused);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(addr), len);
    return true;
}


static NearFloatBlock* AllocateNearPage(uintptr_t moduleBase) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t maxDist = 0x7FFF0000ULL;
    const uintptr_t minAddr = (moduleBase > maxDist) ? (moduleBase - maxDist) : 0x10000;
    const uintptr_t maxAddr = moduleBase + maxDist;

    auto tryAllocFrom = [&](uintptr_t start, bool forward) -> NearFloatBlock* {
        uintptr_t cur = forward
            ? ((start + gran - 1) & ~(gran - 1))
            : (start & ~(gran - 1));
        for (int step = 0; step < 32768; ++step) {
            if (forward && cur >= maxAddr) break;
            if (!forward && cur <= minAddr) break;

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
                break;
            }
            if (mbi.State == MEM_FREE && mbi.RegionSize >= 4096) {
                uintptr_t cand = forward
                    ? ((reinterpret_cast<uintptr_t>(mbi.BaseAddress) + gran - 1) & ~(gran - 1))
                    : ((reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - gran) & ~(gran - 1));
                if (cand >= minAddr && cand <= maxAddr) {
                    void* p = VirtualAlloc(reinterpret_cast<LPVOID>(cand), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (p) {
                        return reinterpret_cast<NearFloatBlock*>(p);
                    }
                }
            }
            if (forward) {
                uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= cur) break;
                cur = (next + gran - 1) & ~(gran - 1);
            } else {
                uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                if (regionBase <= gran || regionBase >= cur) break;
                cur = (regionBase - gran) & ~(gran - 1);
            }
        }
        return nullptr;
    };

    if (NearFloatBlock* p = tryAllocFrom(moduleBase, false)) return p;
    return tryAllocFrom(moduleBase + 0x700000ULL, true);
}

static bool ComputeDisp32(uintptr_t nextIp, const volatile float* target, int32_t& outDisp) {
    const intptr_t diff = reinterpret_cast<intptr_t>(target) - static_cast<intptr_t>(nextIp);
    if (diff < INT32_MIN || diff > INT32_MAX) {
        return false;
    }
    outDisp = static_cast<int32_t>(diff);
    return true;
}

static bool PatchRipDisp32(uintptr_t instrAddr, size_t instrLen, size_t dispOffset, const volatile float* target) {
    int32_t disp = 0;
    if (!ComputeDisp32(instrAddr + instrLen, target, disp)) {
        return false;
    }
    uint8_t buf[4];
    std::memcpy(buf, &disp, sizeof(disp));
    return WriteCodeBytes(instrAddr + dispOffset, buf, sizeof(buf));
}

static void SyncNearFloatsLocked() {
    if (!s_nearBlock) return;
    s_nearBlock->uncondFlipRad = s_cfg.flipAngleDeg * kDegToRad;
    const float effectiveSteerDeg = s_cfg.disableEarlySteerFlip
        ? s_cfg.flipAngleDeg
        : std::min(s_cfg.earlySteerFlipDeg, s_cfg.flipAngleDeg);
    s_nearBlock->steerFlipRad = effectiveSteerDeg * kDegToRad;
    s_nearBlock->centerScale = s_cfg.centerScale;
}

static void ApplyPatchesLocked() {
    if (!s_bytesVerified || !s_nearBlock || !s_nativeBase) return;

    SyncNearFloatsLocked();

    const bool wantMainPatches = s_cfg.enabled;
    if (wantMainPatches && !s_flipPatched) {
        bool ok1 = PatchRipDisp32(s_nativeBase + s_rvaSteerFlipComiss, 7, 3, &s_nearBlock->steerFlipRad);
        bool ok2 = PatchRipDisp32(s_nativeBase + s_rvaUncondFlipComiss, 7, 3, &s_nearBlock->uncondFlipRad);
        bool ok3 = PatchRipDisp32(s_nativeBase + s_rvaCenterScaleMul, 8, 4, &s_nearBlock->centerScale);
        if (ok1 && ok2 && ok3) {
            s_flipPatched = true;
            Logger::Info("SteerAssist: 155-deg vector flip & target scale patches active.");
        } else {
            WriteCodeBytes(s_nativeBase + s_rvaSteerFlipComiss, s_origSteerFlip.data(), s_origSteerFlip.size());
            WriteCodeBytes(s_nativeBase + s_rvaUncondFlipComiss, s_origUncondFlip.data(), s_origUncondFlip.size());
            WriteCodeBytes(s_nativeBase + s_rvaCenterScaleMul, s_origCenterScale.data(), s_origCenterScale.size());
            Logger::Error("SteerAssist: failed to apply RIP-relative displacement patches (range > 2GB).");
        }
    } else if (!wantMainPatches && s_flipPatched) {
        WriteCodeBytes(s_nativeBase + s_rvaSteerFlipComiss, s_origSteerFlip.data(), s_origSteerFlip.size());
        WriteCodeBytes(s_nativeBase + s_rvaUncondFlipComiss, s_origUncondFlip.data(), s_origUncondFlip.size());
        WriteCodeBytes(s_nativeBase + s_rvaCenterScaleMul, s_origCenterScale.data(), s_origCenterScale.size());
        s_flipPatched = false;
        Logger::Info("SteerAssist: vector flip & scale patches restored to vanilla.");
    }

    const bool wantSlipPatch = s_cfg.enabled && s_cfg.fixSlipGate;
    if (wantSlipPatch && !s_slipPatched) {
        const uint8_t jmpMode0[2] = { 0xEB, 0x40 };
        const uint8_t nops[2] = { 0x90, 0x90 };
        bool ok1 = WriteCodeBytes(s_nativeBase + s_rvaMode0SlipJump, jmpMode0, 2);
        bool ok2 = WriteCodeBytes(s_nativeBase + s_rvaSlipGatePosJump, nops, 2);
        bool ok3 = WriteCodeBytes(s_nativeBase + s_rvaSlipGateNegJump, nops, 2);
        if (ok1 && ok2 && ok3) {
            s_slipPatched = true;
            Logger::Info("SteerAssist: symmetric front-slip gate patch active (removed -acos lockup).");
        } else {
            WriteCodeBytes(s_nativeBase + s_rvaMode0SlipJump, s_origMode0Jump.data(), s_origMode0Jump.size());
            WriteCodeBytes(s_nativeBase + s_rvaSlipGatePosJump, s_origPosGateJump.data(), s_origPosGateJump.size());
            WriteCodeBytes(s_nativeBase + s_rvaSlipGateNegJump, s_origNegGateJump.data(), s_origNegGateJump.size());
        }
    } else if (!wantSlipPatch && s_slipPatched) {
        WriteCodeBytes(s_nativeBase + s_rvaMode0SlipJump, s_origMode0Jump.data(), s_origMode0Jump.size());
        WriteCodeBytes(s_nativeBase + s_rvaSlipGatePosJump, s_origPosGateJump.data(), s_origPosGateJump.size());
        WriteCodeBytes(s_nativeBase + s_rvaSlipGateNegJump, s_origNegGateJump.data(), s_origNegGateJump.size());
        s_slipPatched = false;
        Logger::Info("SteerAssist: front-slip gate restored to vanilla.");
    }
}

static bool EnsureInitializedLocked() {
    if (s_initialized) return true;

    HMODULE hNative = GetModuleHandleW(L"NativeCarX.dll");
    if (!hNative) {
        return false;
    }

    s_nativeFound = true;
    s_nativeBase = reinterpret_cast<uintptr_t>(hNative);

    // Dynamically scan for patch sites in NativeCarX.dll with signature scans
    uintptr_t vaCenter = PatternScanner::Find(hNative, "0F 28 C4 F3 0F 59 05 ? ? ? ? F3 41 0F 5D D0", "SteerAssist_CenterScaleMul");
    if (vaCenter) s_rvaCenterScaleMul = vaCenter + 3 - s_nativeBase;

    uintptr_t vaMode0 = PatternScanner::Find(hNative, "F3 0F 11 8B F8 00 00 00 76 40 44 0F 2F 15", "SteerAssist_Mode0SlipJump");
    if (vaMode0) s_rvaMode0SlipJump = vaMode0 + 8 - s_nativeBase;

    uintptr_t vaPosGate = PatternScanner::Find(hNative, "F3 0F 59 15 ? ? ? ? 76 1B F3 0F 10 05", "SteerAssist_SlipGatePosJump");
    if (vaPosGate) s_rvaSlipGatePosJump = vaPosGate + 8 - s_nativeBase;

    uintptr_t vaNegGate = PatternScanner::Find(hNative, "45 0F 2F D4 73 1B F3 0F 10 05", "SteerAssist_SlipGateNegJump");
    if (vaNegGate) s_rvaSlipGateNegJump = vaNegGate + 4 - s_nativeBase;

    uintptr_t vaSteerFlip = PatternScanner::Find(hNative, "0F 28 C8 0F 54 CA 0F 2F 0D ? ? ? ? 76 25", "SteerAssist_SteerFlipComiss");
    if (vaSteerFlip) s_rvaSteerFlipComiss = vaSteerFlip + 6 - s_nativeBase;

    uintptr_t vaUncondFlip = PatternScanner::Find(hNative, "B3 01 41 88 1F 0F 2F 0D ? ? ? ? 0F B6 DB", "SteerAssist_UncondFlipComiss");
    if (vaUncondFlip) s_rvaUncondFlipComiss = vaUncondFlip + 5 - s_nativeBase;

    // Cache original bytes dynamically
    memcpy(s_origCenterScale.data(), reinterpret_cast<const void*>(s_nativeBase + s_rvaCenterScaleMul), s_origCenterScale.size());
    memcpy(s_origMode0Jump.data(), reinterpret_cast<const void*>(s_nativeBase + s_rvaMode0SlipJump), s_origMode0Jump.size());
    memcpy(s_origPosGateJump.data(), reinterpret_cast<const void*>(s_nativeBase + s_rvaSlipGatePosJump), s_origPosGateJump.size());
    memcpy(s_origNegGateJump.data(), reinterpret_cast<const void*>(s_nativeBase + s_rvaSlipGateNegJump), s_origNegGateJump.size());
    memcpy(s_origSteerFlip.data(), reinterpret_cast<const void*>(s_nativeBase + s_rvaSteerFlipComiss), s_origSteerFlip.size());
    memcpy(s_origUncondFlip.data(), reinterpret_cast<const void*>(s_nativeBase + s_rvaUncondFlipComiss), s_origUncondFlip.size());

    // Validate opcode signatures before writing any patches
    bool bytesValid = (s_origCenterScale[0] == 0xF3 && s_origCenterScale[1] == 0x0F && s_origCenterScale[2] == 0x59) &&
                      (s_origMode0Jump[0] == 0x76 || s_origMode0Jump[0] == 0xEB) &&
                      (s_origPosGateJump[0] == 0x76 || s_origPosGateJump[0] == 0x90) &&
                      (s_origNegGateJump[0] == 0x73 || s_origNegGateJump[0] == 0x90) &&
                      (s_origSteerFlip[0] == 0x0F && s_origSteerFlip[1] == 0x2F) &&
                      (s_origUncondFlip[0] == 0x0F && s_origUncondFlip[1] == 0x2F);

    if (!bytesValid) {
        Logger::Warn("SteerAssist: NativeCarX.dll opcodes could not be verified (game updated?). Patches disabled safely.");
        s_bytesVerified = false;
        s_initialized = true;
        return false;
    }
    s_bytesVerified = true;

    s_nearBlock = AllocateNearPage(s_nativeBase);
    if (!s_nearBlock) {
        Logger::Error("SteerAssist: failed to allocate memory within +/-2GB of NativeCarX.dll.");
        s_nearAllocOk = false;
        s_initialized = true;
        return false;
    }
    s_nearAllocOk = true;
    s_initialized = true;

    ApplyPatchesLocked();
    return true;
}

} // namespace

bool SteerAssist::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return EnsureInitializedLocked();
}

void SteerAssist::Shutdown() {
    NearFloatBlock* pageToFree = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (!s_initialized) return;

        bool wasPatched = s_flipPatched || s_slipPatched;
        if (s_bytesVerified && s_nativeBase) {
            if (s_slipPatched) {
                WriteCodeBytes(s_nativeBase + s_rvaMode0SlipJump, s_origMode0Jump.data(), s_origMode0Jump.size());
                WriteCodeBytes(s_nativeBase + s_rvaSlipGatePosJump, s_origPosGateJump.data(), s_origPosGateJump.size());
                WriteCodeBytes(s_nativeBase + s_rvaSlipGateNegJump, s_origNegGateJump.data(), s_origNegGateJump.size());
                s_slipPatched = false;
            }
            if (s_flipPatched) {
                WriteCodeBytes(s_nativeBase + s_rvaSteerFlipComiss, s_origSteerFlip.data(), s_origSteerFlip.size());
                WriteCodeBytes(s_nativeBase + s_rvaUncondFlipComiss, s_origUncondFlip.data(), s_origUncondFlip.size());
                WriteCodeBytes(s_nativeBase + s_rvaCenterScaleMul, s_origCenterScale.data(), s_origCenterScale.size());
                s_flipPatched = false;
            }
        }

        pageToFree = s_nearBlock;
        s_nearBlock = nullptr;
        s_nearAllocOk = false;
        s_bytesVerified = false;
        s_nativeFound = false;
        s_nativeBase = 0;
        s_initialized = false;

        if (wasPatched) {
            // Allow any physics tick currently inside 0x8DA0/0x9DD0 to finish reading s_nearBlock before VirtualFree
            Sleep(40);
        }
    }

    if (pageToFree) {
        VirtualFree(pageToFree, 0, MEM_RELEASE);
    }
    Logger::Info("SteerAssist module shut down cleanly.");
}

SteerAssist::Config SteerAssist::GetConfig() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_cfg;
}

void SteerAssist::SetConfig(const Config& cfg) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    Config clamped = cfg;
    clamped.flipAngleDeg = std::clamp(clamped.flipAngleDeg, 90.0f, 180.0f);
    clamped.earlySteerFlipDeg = std::clamp(clamped.earlySteerFlipDeg, 90.0f, clamped.flipAngleDeg);
    clamped.centerScale = std::clamp(clamped.centerScale, 0.15f, 0.85f);
    s_cfg = clamped;

    EnsureInitializedLocked();
    ApplyPatchesLocked();
}

void SteerAssist::SetRecommendedDefaults() {
    Config cfg{};
    cfg.enabled = true;
    cfg.flipAngleDeg = 155.0f;
    cfg.disableEarlySteerFlip = true;
    cfg.earlySteerFlipDeg = 135.0f;
    cfg.fixSlipGate = true;
    cfg.centerScale = 0.30f;
    SetConfig(cfg);
}

void SteerAssist::SetVanillaDefaults() {
    Config cfg{};
    cfg.enabled = false;
    cfg.flipAngleDeg = 140.0f;
    cfg.disableEarlySteerFlip = false;
    cfg.earlySteerFlipDeg = 90.0f;
    cfg.fixSlipGate = false;
    cfg.centerScale = 0.30f;
    SetConfig(cfg);
}

SteerAssist::Stats SteerAssist::GetStats() {
    Stats st{};
    uintptr_t carAddr = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        EnsureInitializedLocked();
        st.initialized = s_initialized;
        st.nativeFound = s_nativeFound;
        st.bytesVerified = s_bytesVerified;
        st.nearAllocOk = s_nearAllocOk;
        st.patchesApplied = s_flipPatched || s_slipPatched;
        st.nativeBase = s_nativeBase;
        st.nearDataAddr = reinterpret_cast<uintptr_t>(s_nearBlock);
    }

    void* carPtr = NativeCarX::GetPlayerCarPtr();
    carAddr = reinterpret_cast<uintptr_t>(carPtr);
    if (carAddr >= 0x10000) {
        int32_t mode = 0, ffMode = 0, active = 0;
        float speedMps = 0.0f, maxSteerRad = 0.0f, ffb = 0.0f;
        if (SafeReadValue(carAddr + kOffSteerMode, mode) &&
            SafeReadValue(carAddr + kOffSteerFFMode, ffMode) &&
            SafeReadValue(carAddr + kOffAssistActive, active) &&
            SafeReadValue(carAddr + kOffSpeedMps, speedMps) &&
            SafeReadValue(carAddr + kOffMaxSteerRad, maxSteerRad) &&
            SafeReadValue(carAddr + kOffFFBTorque, ffb))
        {
            st.hasCar = true;
            st.steerMode = mode;
            st.steerFFmode = ffMode;
            st.assistActive = active;
            st.speedKmh = speedMps * 3.6f;
            st.maxSteerDeg = maxSteerRad * kRadToDeg;
            st.ffbTorque = ffb;

            CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
            st.steerDeg = tele.steerAngle * kRadToDeg;

            int32_t flIdx = -1, frIdx = -1;
            if (SafeReadValue(carAddr + kOffWheelFLIndex, flIdx) &&
                SafeReadValue(carAddr + kOffWheelFRIndex, frIdx) &&
                flIdx >= 0 && flIdx <= 7 && frIdx >= 0 && frIdx <= 7)
            {
                const uintptr_t flBase = carAddr + kOffWheelsBase + static_cast<uintptr_t>(flIdx) * kWheelStride;
                const uintptr_t frBase = carAddr + kOffWheelsBase + static_cast<uintptr_t>(frIdx) * kWheelStride;
                uint8_t hasGate = 0;
                float flSa = 0.0f, frSa = 0.0f;
                if (SafeReadValue(flBase + kOffWheelHasSlipGate, hasGate) && hasGate != 0 &&
                    SafeReadValue(flBase + kOffWheelSlipAngleNeg, flSa) &&
                    SafeReadValue(frBase + kOffWheelSlipAngleNeg, frSa))
                {
                    st.frontSlipDeg = 0.5f * (std::fabs(flSa) + std::fabs(frSa)) * kRadToDeg;
                }
            }
        }
    }
    return st;
}
