#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

class SteerAssist {
public:
    struct Config {
        bool enabled = true;
        float flipAngleDeg = 155.0f;         // Vanilla: 140.0 deg (unconditional 180° flip threshold)
        bool disableEarlySteerFlip = true;   // Vanilla: false (90° flip when full steer matches slide)
        float earlySteerFlipDeg = 135.0f;    // Used if disableEarlySteerFlip == false (Vanilla: 90.0 deg)
        bool fixSlipGate = true;             // Removes broken -acos <= 0 ±15.6° front slip gate in Assist2
        float centerScale = 0.30f;           // Target center angle multiplier in Assist2 (Vanilla: 0.30f)
    };

    struct Stats {
        bool initialized = false;
        bool nativeFound = false;
        bool bytesVerified = false;
        bool nearAllocOk = false;
        bool patchesApplied = false;
        uintptr_t nativeBase = 0;
        uintptr_t nearDataAddr = 0;

        bool hasCar = false;
        int32_t steerMode = 0;
        int32_t steerFFmode = 0;
        int32_t assistActive = 0;
        float speedKmh = 0.0f;
        float steerDeg = 0.0f;
        float maxSteerDeg = 0.0f;
        float ffbTorque = 0.0f;
        float frontSlipDeg = 0.0f;
    };

    static bool Init();
    static void Shutdown();

    static Config GetConfig();
    static void SetConfig(const Config& cfg);
    static void SetRecommendedDefaults();
    static void SetVanillaDefaults();

    static Stats GetStats();
};
