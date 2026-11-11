#pragma once
#include <windows.h>
#include <vector>
#include <mutex>
#include <array>
#include <chrono>

struct WheelData {
    std::array<float, 16> matrix;
    std::array<float, 16> caliperMatrix;
    float smoke = 0.0f;
    float smokeIntensity = 0.0f;
    float camber = 0.0f;
    float toeIn = 0.0f;
    float wearFactor = 0.0f;
    float deformAngle = 0.0f;
    float zoneRest = 1.0f;
};

struct TyreConfig {
    bool infiniteWear = false;
    float wearMultiplier = 1.0f;
    float desiredLife = 1.0f; // 0.0 .. 1.0 (1.0 = 100% fresh)
    bool overrideGrip = false;
    float frontGrip = 1.15f;
    float rearGrip = 0.95f;
    float ballisticGrip = 0.10f;
    bool lockTemperature = false;
    float targetTemperature = 80.0f;
    bool overridePressure = false;
    float frontPressure = 145.0f; // in kPa
    float rearPressure = 95.0f;   // in kPa
    float baselineFrontPressure = 145.0f; // in kPa (captured from car setup)
    float baselineRearPressure = 95.0f;   // in kPa (captured from car setup)
};

struct TyreWheelLive {
    float life = 1.0f;
    float temp = 80.0f;
    float pressureKpa = 145.0f;
    float friction = 1.15f;
    float ballisticGrip = 0.10f;
    float wearFactor = 1.0f;
};

struct TyreLiveStats {
    bool valid = false;
    std::array<TyreWheelLive, 4> wheels;
};

struct CarTelemetry {
    void* carPtr = nullptr;
    std::array<float, 16> transformMatrix;
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float speedKmh = 0.0f;
    float rpm = 0.0f;
    int gear = 0;
    float steerAngle = 0.0f;
    float accel = 0.0f;
    float brake = 0.0f;
    float handbrake = 0.0f;
    float clutch = 0.0f;
    int wheelCount = 4;
    std::array<WheelData, 4> wheels;
    bool isLocalPlayer = false;
    std::chrono::steady_clock::time_point lastUpdateTime;
};

class NativeCarX {
public:
    static bool Init();
    static void Shutdown();

    // Telemetry and Tracking
    static CarTelemetry GetPlayerCarTelemetry();
    static std::vector<CarTelemetry> GetAllCarsTelemetry();
    static bool IsInSession();

    // Direct Car Controls
    static void SetCarRenderMatrix(void* car, const float* matrix16, float factor = 1.0f);
    static void SetCarActive(void* car, bool active);
    static void SetCarRPM(void* car, float rpm);
    static void SetCarGear(void* car, int gear);
    static bool GetCarRigidBody(void* car, float outRb[29]);
    static bool SetCarRigidBody(void* car, const float inRb[29]);
    static bool GetCarVelocity(void* car, float outVel[3]);
    static bool GetCarAngularVelocity(void* car, float outAngVel[3]);
    static void* SpawnCar();
    static bool DestroyCar(void* car);
    static void* GetPlayerCarPtr();
    static void SetKnownPlayerCarPtr(void* car);
    static void* GetManagerPtr();

    // Damage & Tire Wear Control
    static void SetNoDamage(bool enable);
    static bool IsNoDamage();
    static void SetNoTireWear(bool enable);
    static bool IsNoTireWear();
    static void ResetCarDamage(void* car = nullptr);
    static void ResetTireWear(void* car = nullptr);
    static float GetWheelWear(void* car, int wheelIndex);
    static void UpdatePerFrame();

    // Tire & Grip Tuning API
    static TyreConfig GetTyreConfig();
    static void SetTyreConfig(const TyreConfig& cfg);
    static TyreLiveStats GetTyreLiveStats();
    static void ResetTyreDefaults();

private:
    static void Hooked_CX_CarSetRenderMatrix(void* car, const float* matrix, float factor);
    typedef void (*t_CX_CarSetRenderMatrix)(void* car, const float* matrix, float factor);
    static t_CX_CarSetRenderMatrix s_originalSetRenderMatrix;

    static void* Hooked_CX_CreateCar(void* manager);
    typedef void* (*t_CX_CreateCar)(void* manager);
    static t_CX_CreateCar s_originalCreateCar;

    static void Hooked_CX_DestroyCar(void* manager, void* car);
    typedef void (*t_CX_DestroyCar)(void* manager, void* car);
    static t_CX_DestroyCar s_originalDestroyCar;

    static void Hooked_CX_DestroyManager(void* manager);
    typedef void (*t_CX_DestroyManager)(void* manager);
    static t_CX_DestroyManager s_originalDestroyManager;

    static void PruneStaleCarsLocked();
    static bool IsCarMemoryValid(void* car);
    static void UpdateCarTelemetry(CarTelemetry& tele);

    static std::recursive_mutex s_mutex;
    static std::vector<CarTelemetry> s_trackedCars;
    static void* s_playerCarPtr;
    static void* s_knownPlayerCarPtr;
    static void* s_managerPtr;
    static HMODULE s_nativeCarXModule;
    static bool s_initialized;

    // Function pointers
    typedef float (*t_CX_CarGetSpeedKMH)(void* car);
    typedef float (*t_CX_CarGetRPM)(void* car);
    typedef int   (*t_CX_CarGetGear)(void* car);
    typedef float (*t_CX_CarGetSteerAngle)(void* car);
    typedef float (*t_CX_CarGetAccel)(void* car);
    typedef float (*t_CX_CarGetBrake)(void* car);
    typedef float (*t_CX_CarGetHandBrake)(void* car);
    typedef float (*t_CX_CarGetClutch)(void* car);
    typedef void* (*t_CX_CarGetWheel)(void* car, int wheelIndex);
    typedef void  (*t_CX_WheelGetWheelMatrix)(void* wheel, float* outMatrix);
    typedef void  (*t_CX_WheelGetCaliperMatrix)(void* wheel, float* outMatrix);
    typedef float (*t_CX_WheelGetSmoke)(void* wheel);
    typedef float (*t_CX_WheelGetSmokeIntensity)(void* wheel);
    typedef float (*t_CX_WheelGetCamber)(void* wheel);
    typedef float (*t_CX_WheelGetToeIn)(void* wheel);

    typedef void  (*t_CX_CarSetActive)(void* car, int active);
    typedef void  (*t_CX_CarSetRPM)(void* car, float rpm);
    typedef void  (*t_CX_CarSetGear)(void* car, int gear);
    typedef void  (*t_CX_CarGetRigidBody)(void* car, void* outRigidBody);
    typedef void  (*t_CX_CarSetRigidBody)(void* car, const void* inRigidBody);
    typedef void  (*t_CX_CarGetVelocity)(void* car, float* outVec3);

    static t_CX_CarGetSpeedKMH pfn_GetSpeedKMH;
    static t_CX_CarGetRPM pfn_GetRPM;
    static t_CX_CarGetGear pfn_GetGear;
    static t_CX_CarGetSteerAngle pfn_GetSteerAngle;
    static t_CX_CarGetAccel pfn_GetAccel;
    static t_CX_CarGetBrake pfn_GetBrake;
    static t_CX_CarGetHandBrake pfn_GetHandBrake;
    static t_CX_CarGetClutch pfn_GetClutch;
    static t_CX_CarGetWheel pfn_GetWheel;
    static t_CX_WheelGetWheelMatrix pfn_GetWheelMatrix;
    static t_CX_WheelGetCaliperMatrix pfn_GetCaliperMatrix;
    static t_CX_WheelGetSmoke pfn_GetSmoke;
    static t_CX_WheelGetSmokeIntensity pfn_GetSmokeIntensity;
    static t_CX_WheelGetCamber pfn_GetCamber;
    static t_CX_WheelGetToeIn pfn_GetToeIn;

    static t_CX_CarSetActive pfn_CarSetActive;
    static t_CX_CarSetRPM pfn_CarSetRPM;
    static t_CX_CarSetGear pfn_CarSetGear;
    static t_CX_CreateCar pfn_CreateCar;
    static t_CX_DestroyCar pfn_DestroyCar;
    static t_CX_CarGetRigidBody pfn_GetRigidBody;
    static t_CX_CarSetRigidBody pfn_SetRigidBody;
    static t_CX_CarGetVelocity pfn_GetVelocity;
    static t_CX_CarGetVelocity pfn_GetAngularVelocity;

    typedef void  (*t_CX_CarSetTyreWearMode)(void* car, int mode);
    typedef int   (*t_CX_CarGetTyreWearMode)(void* car);
    typedef void  (*t_CX_WheelTyreWearReset)(void* wheel, float initialLife);
    typedef void  (*t_CX_WheelTyreTempReset)(void* wheel);
    typedef void  (*t_CX_WheelSetWearFactor)(void* wheel, float factor);
    typedef float (*t_CX_WheelGetWearFactor)(void* wheel);
    typedef float (*t_CX_WheelGetZoneRest)(void* wheel, int zoneIndex);
    typedef void  (*t_CX_WheelSetDeformOffset)(void* wheel, float offset);
    typedef void  (*t_CX_WheelSetDeformAngle)(void* wheel, float angle);
    typedef float (*t_CX_WheelGetDeformAngle)(void* wheel);

    static t_CX_CarSetTyreWearMode pfn_CarSetTyreWearMode;
    static t_CX_CarGetTyreWearMode pfn_CarGetTyreWearMode;
    static t_CX_WheelTyreWearReset pfn_WheelTyreWearReset;
    static t_CX_WheelTyreTempReset pfn_WheelTyreTempReset;
    static t_CX_WheelSetWearFactor pfn_WheelSetWearFactor;
    static t_CX_WheelGetWearFactor pfn_WheelGetWearFactor;
    static t_CX_WheelGetZoneRest pfn_WheelGetZoneRest;
    static t_CX_WheelSetDeformOffset pfn_WheelSetDeformOffset;
    static t_CX_WheelSetDeformAngle pfn_WheelSetDeformAngle;
    static t_CX_WheelGetDeformAngle pfn_WheelGetDeformAngle;

    typedef float (*t_CX_WheelGetTyrePressure)(void* wheel);
    typedef void  (*t_CX_WheelSetTyrePressure)(void* wheel, float pressure);
    typedef float (*t_CX_WheelGetZoneTemp)(void* wheel, int zoneIndex);
    typedef int   (*t_CX_WheelGetTyreWearZones)(void* wheel);
    typedef float (*t_CX_WheelGetFrictionMultiplier)(void* wheel);
    typedef void  (*t_CX_WheelSetFrictionMultiplier)(void* wheel, float mult);
    typedef float (*t_CX_WheelGetBallisticGripFactor)(void* wheel);
    typedef void  (*t_CX_WheelSetBallisticGripFactor)(void* wheel, float factor);

    static t_CX_WheelGetTyrePressure pfn_WheelGetTyrePressure;
    static t_CX_WheelSetTyrePressure pfn_WheelSetTyrePressure;
    static t_CX_WheelGetZoneTemp pfn_WheelGetZoneTemp;
    static t_CX_WheelGetTyreWearZones pfn_WheelGetTyreWearZones;
    static t_CX_WheelGetFrictionMultiplier pfn_WheelGetFrictionMultiplier;
    static t_CX_WheelSetFrictionMultiplier pfn_WheelSetFrictionMultiplier;
    static t_CX_WheelGetBallisticGripFactor pfn_WheelGetBallisticGripFactor;
    static t_CX_WheelSetBallisticGripFactor pfn_WheelSetBallisticGripFactor;

    static TyreConfig s_tyreConfig;
    static float s_baselineFrontPressure;
    static float s_baselineRearPressure;
    static void* s_lastBaselineCar;
    static bool s_noDamage;
    static bool s_noTireWear;
};
