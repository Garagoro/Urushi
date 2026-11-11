#include "steer_assist.h"
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

// Verified RVAs inside NativeCarX.dll (x64, ImageBase 0x180000000)
constexpr uintptr_t kRvaCenterScaleMul    = 0x90DE; // F3 0F 59 05 [disp32]  mulss xmm0, [0x96310] (0.3f)
constexpr uintptr_t kRvaMode0SlipJump     = 0x91F9; // 76 40                  jbe 0x923B
constexpr uintptr_t kRvaSlipGatePosJump   = 0x92C8; // 76 1B                  jbe 0x92E5 (-1.3f gate)
constexpr uintptr_t kRvaSlipGateNegJump   = 0x92E9; // 73 1B                  jae 0x9306 (+1.3f gate)
constexpr uintptr_t kRvaSteerFlipComiss   = 0xA5C5; // 0F 2F 0D [disp32]      comiss xmm1, [0x96420] (90 deg)
constexpr uintptr_t kRvaUncondFlipComiss  = 0xA5F3; // 0F 2F 0D [disp32]      comiss xmm1, [0x96488] (140 deg)

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

constexpr std::array<uint8_t, 8> kOrigCenterScale = { 0xF3, 0x0F, 0x59, 0x05, 0x2A, 0xD2, 0x08, 0x00 };
constexpr std::array<uint8_t, 2> kOrigMode0Jump   = { 0x76, 0x40 };
constexpr std::array<uint8_t, 2> kOrigPosGateJump = { 0x76, 0x1B };
constexpr std::array<uint8_t, 2> kOrigNegGateJump = { 0x73, 0x1B };
constexpr std::array<uint8_t, 7> kOrigSteerFlip   = { 0x0F, 0x2F, 0x0D, 0x54, 0xBE, 0x08, 0x00 };
constexpr std::array<uint8_t, 7> kOrigUncondFlip  = { 0x0F, 0x2F, 0x0D, 0x8E, 0xBE, 0x08, 0x00 };

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

static bool VerifyAllOriginalBytes(uintptr_t base) {
    auto eq = [base](uintptr_t rva, const uint8_t* expected, size_t len) {
        return std::memcmp(reinterpret_cast<const void*>(base + rva), expected, len) == 0;
    };
    return eq(kRvaCenterScaleMul, kOrigCenterScale.data(), kOrigCenterScale.size()) &&
           eq(kRvaMode0SlipJump, kOrigMode0Jump.data(), kOrigMode0Jump.size()) &&
           eq(kRvaSlipGatePosJump, kOrigPosGateJump.data(), kOrigPosGateJump.size()) &&
           eq(kRvaSlipGateNegJump, kOrigNegGateJump.data(), kOrigNegGateJump.size()) &&
           eq(kRvaSteerFlipComiss, kOrigSteerFlip.data(), kOrigSteerFlip.size()) &&
           eq(kRvaUncondFlipComiss, kOrigUncondFlip.data(), kOrigUncondFlip.size());
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
        bool ok1 = PatchRipDisp32(s_nativeBase + kRvaSteerFlipComiss, 7, 3, &s_nearBlock->steerFlipRad);
        bool ok2 = PatchRipDisp32(s_nativeBase + kRvaUncondFlipComiss, 7, 3, &s_nearBlock->uncondFlipRad);
        bool ok3 = PatchRipDisp32(s_nativeBase + kRvaCenterScaleMul, 8, 4, &s_nearBlock->centerScale);
        if (ok1 && ok2 && ok3) {
            s_flipPatched = true;
            Logger::Info("SteerAssist: 155-deg vector flip & target scale patches active.");
        } else {
            WriteCodeBytes(s_nativeBase + kRvaSteerFlipComiss, kOrigSteerFlip.data(), kOrigSteerFlip.size());
            WriteCodeBytes(s_nativeBase + kRvaUncondFlipComiss, kOrigUncondFlip.data(), kOrigUncondFlip.size());
            WriteCodeBytes(s_nativeBase + kRvaCenterScaleMul, kOrigCenterScale.data(), kOrigCenterScale.size());
            Logger::Error("SteerAssist: failed to apply RIP-relative displacement patches (range > 2GB).");
        }
    } else if (!wantMainPatches && s_flipPatched) {
        WriteCodeBytes(s_nativeBase + kRvaSteerFlipComiss, kOrigSteerFlip.data(), kOrigSteerFlip.size());
        WriteCodeBytes(s_nativeBase + kRvaUncondFlipComiss, kOrigUncondFlip.data(), kOrigUncondFlip.size());
        WriteCodeBytes(s_nativeBase + kRvaCenterScaleMul, kOrigCenterScale.data(), kOrigCenterScale.size());
        s_flipPatched = false;
        Logger::Info("SteerAssist: vector flip & scale patches restored to vanilla.");
    }

    const bool wantSlipPatch = s_cfg.enabled && s_cfg.fixSlipGate;
    if (wantSlipPatch && !s_slipPatched) {
        const uint8_t jmpMode0[2] = { 0xEB, 0x40 };
        const uint8_t nops[2] = { 0x90, 0x90 };
        bool ok1 = WriteCodeBytes(s_nativeBase + kRvaMode0SlipJump, jmpMode0, 2);
        bool ok2 = WriteCodeBytes(s_nativeBase + kRvaSlipGatePosJump, nops, 2);
        bool ok3 = WriteCodeBytes(s_nativeBase + kRvaSlipGateNegJump, nops, 2);
        if (ok1 && ok2 && ok3) {
            s_slipPatched = true;
            Logger::Info("SteerAssist: symmetric front-slip gate patch active (removed -acos lockup).");
        } else {
            WriteCodeBytes(s_nativeBase + kRvaMode0SlipJump, kOrigMode0Jump.data(), kOrigMode0Jump.size());
            WriteCodeBytes(s_nativeBase + kRvaSlipGatePosJump, kOrigPosGateJump.data(), kOrigPosGateJump.size());
            WriteCodeBytes(s_nativeBase + kRvaSlipGateNegJump, kOrigNegGateJump.data(), kOrigNegGateJump.size());
        }
    } else if (!wantSlipPatch && s_slipPatched) {
        WriteCodeBytes(s_nativeBase + kRvaMode0SlipJump, kOrigMode0Jump.data(), kOrigMode0Jump.size());
        WriteCodeBytes(s_nativeBase + kRvaSlipGatePosJump, kOrigPosGateJump.data(), kOrigPosGateJump.size());
        WriteCodeBytes(s_nativeBase + kRvaSlipGateNegJump, kOrigNegGateJump.data(), kOrigNegGateJump.size());
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

    if (!VerifyAllOriginalBytes(s_nativeBase)) {
        Logger::Warn("SteerAssist: NativeCarX.dll bytes did not match expected offsets (game updated?). Patches disabled for safety.");
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
                WriteCodeBytes(s_nativeBase + kRvaMode0SlipJump, kOrigMode0Jump.data(), kOrigMode0Jump.size());
                WriteCodeBytes(s_nativeBase + kRvaSlipGatePosJump, kOrigPosGateJump.data(), kOrigPosGateJump.size());
                WriteCodeBytes(s_nativeBase + kRvaSlipGateNegJump, kOrigNegGateJump.data(), kOrigNegGateJump.size());
                s_slipPatched = false;
            }
            if (s_flipPatched) {
                WriteCodeBytes(s_nativeBase + kRvaSteerFlipComiss, kOrigSteerFlip.data(), kOrigSteerFlip.size());
                WriteCodeBytes(s_nativeBase + kRvaUncondFlipComiss, kOrigUncondFlip.data(), kOrigUncondFlip.size());
                WriteCodeBytes(s_nativeBase + kRvaCenterScaleMul, kOrigCenterScale.data(), kOrigCenterScale.size());
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
