#include "native_carx.h"
#include "hook_manager.h"
#include "logger.h"
#include <algorithm>

std::recursive_mutex NativeCarX::s_mutex;
std::vector<CarTelemetry> NativeCarX::s_trackedCars;
void* NativeCarX::s_playerCarPtr = nullptr;
void* NativeCarX::s_managerPtr = nullptr;
HMODULE NativeCarX::s_nativeCarXModule = nullptr;
bool NativeCarX::s_initialized = false;

NativeCarX::t_CX_CarSetRenderMatrix NativeCarX::s_originalSetRenderMatrix = nullptr;
NativeCarX::t_CX_CreateCar NativeCarX::s_originalCreateCar = nullptr;
NativeCarX::t_CX_DestroyCar NativeCarX::s_originalDestroyCar = nullptr;
NativeCarX::t_CX_DestroyManager NativeCarX::s_originalDestroyManager = nullptr;
NativeCarX::t_CX_CarGetSpeedKMH NativeCarX::pfn_GetSpeedKMH = nullptr;
NativeCarX::t_CX_CarGetRPM NativeCarX::pfn_GetRPM = nullptr;
NativeCarX::t_CX_CarGetGear NativeCarX::pfn_GetGear = nullptr;
NativeCarX::t_CX_CarGetSteerAngle NativeCarX::pfn_GetSteerAngle = nullptr;
NativeCarX::t_CX_CarGetAccel NativeCarX::pfn_GetAccel = nullptr;
NativeCarX::t_CX_CarGetBrake NativeCarX::pfn_GetBrake = nullptr;
NativeCarX::t_CX_CarGetHandBrake NativeCarX::pfn_GetHandBrake = nullptr;
NativeCarX::t_CX_CarGetClutch NativeCarX::pfn_GetClutch = nullptr;
NativeCarX::t_CX_CarGetWheel NativeCarX::pfn_GetWheel = nullptr;
NativeCarX::t_CX_WheelGetWheelMatrix NativeCarX::pfn_GetWheelMatrix = nullptr;
NativeCarX::t_CX_WheelGetCaliperMatrix NativeCarX::pfn_GetCaliperMatrix = nullptr;
NativeCarX::t_CX_WheelGetSmoke NativeCarX::pfn_GetSmoke = nullptr;
NativeCarX::t_CX_WheelGetSmokeIntensity NativeCarX::pfn_GetSmokeIntensity = nullptr;
NativeCarX::t_CX_WheelGetCamber NativeCarX::pfn_GetCamber = nullptr;
NativeCarX::t_CX_WheelGetToeIn NativeCarX::pfn_GetToeIn = nullptr;

NativeCarX::t_CX_CarSetActive NativeCarX::pfn_CarSetActive = nullptr;
NativeCarX::t_CX_CarSetRPM NativeCarX::pfn_CarSetRPM = nullptr;
NativeCarX::t_CX_CarSetGear NativeCarX::pfn_CarSetGear = nullptr;
NativeCarX::t_CX_CreateCar NativeCarX::pfn_CreateCar = nullptr;
NativeCarX::t_CX_DestroyCar NativeCarX::pfn_DestroyCar = nullptr;
NativeCarX::t_CX_CarGetRigidBody NativeCarX::pfn_GetRigidBody = nullptr;
NativeCarX::t_CX_CarSetRigidBody NativeCarX::pfn_SetRigidBody = nullptr;
NativeCarX::t_CX_CarGetVelocity NativeCarX::pfn_GetVelocity = nullptr;

bool NativeCarX::Init() {
    if (s_initialized) return true;

    s_nativeCarXModule = GetModuleHandleW(L"NativeCarX.dll");
    if (!s_nativeCarXModule) {
        Logger::Warn("NativeCarX.dll not yet loaded, will retry...");
        return false;
    }

    // Resolve functions
    pfn_GetSpeedKMH = (t_CX_CarGetSpeedKMH)GetProcAddress(s_nativeCarXModule, "CX_CarGetSpeedKMH");
    pfn_GetRPM = (t_CX_CarGetRPM)GetProcAddress(s_nativeCarXModule, "CX_CarGetRPM");
    pfn_GetGear = (t_CX_CarGetGear)GetProcAddress(s_nativeCarXModule, "CX_CarGetGear");
    pfn_GetSteerAngle = (t_CX_CarGetSteerAngle)GetProcAddress(s_nativeCarXModule, "CX_CarGetSteerAngle");
    pfn_GetAccel = (t_CX_CarGetAccel)GetProcAddress(s_nativeCarXModule, "CX_CarGetAccel");
    pfn_GetBrake = (t_CX_CarGetBrake)GetProcAddress(s_nativeCarXModule, "CX_CarGetBrake");
    pfn_GetHandBrake = (t_CX_CarGetHandBrake)GetProcAddress(s_nativeCarXModule, "CX_CarGetHandBrake");
    pfn_GetClutch = (t_CX_CarGetClutch)GetProcAddress(s_nativeCarXModule, "CX_CarGetClutch");
    pfn_GetWheel = (t_CX_CarGetWheel)GetProcAddress(s_nativeCarXModule, "CX_CarGetWheel");
    pfn_GetWheelMatrix = (t_CX_WheelGetWheelMatrix)GetProcAddress(s_nativeCarXModule, "CX_WheelGetWheelMatrix");
    pfn_GetCaliperMatrix = (t_CX_WheelGetCaliperMatrix)GetProcAddress(s_nativeCarXModule, "CX_WheelGetCaliperMatrix");
    pfn_GetSmoke = (t_CX_WheelGetSmoke)GetProcAddress(s_nativeCarXModule, "CX_WheelGetSmoke");
    pfn_GetSmokeIntensity = (t_CX_WheelGetSmokeIntensity)GetProcAddress(s_nativeCarXModule, "CX_WheelGetSmokeIntensity");
    pfn_GetCamber = (t_CX_WheelGetCamber)GetProcAddress(s_nativeCarXModule, "CX_WheelGetCamber");
    pfn_GetToeIn = (t_CX_WheelGetToeIn)GetProcAddress(s_nativeCarXModule, "CX_WheelGetToeIn");

    pfn_CarSetActive = (t_CX_CarSetActive)GetProcAddress(s_nativeCarXModule, "CX_CarSetActive");
    pfn_CarSetRPM = (t_CX_CarSetRPM)GetProcAddress(s_nativeCarXModule, "CX_CarSetRPM");
    pfn_CarSetGear = (t_CX_CarSetGear)GetProcAddress(s_nativeCarXModule, "CX_CarSetGear");
    pfn_CreateCar = (t_CX_CreateCar)GetProcAddress(s_nativeCarXModule, "CX_CreateCar");
    pfn_DestroyCar = (t_CX_DestroyCar)GetProcAddress(s_nativeCarXModule, "CX_DestroyCar");
    pfn_GetRigidBody = (t_CX_CarGetRigidBody)GetProcAddress(s_nativeCarXModule, "CX_CarGetRigidBody");
    pfn_SetRigidBody = (t_CX_CarSetRigidBody)GetProcAddress(s_nativeCarXModule, "CX_CarSetRigidBody");
    pfn_GetVelocity = (t_CX_CarGetVelocity)GetProcAddress(s_nativeCarXModule, "CX_CarGetVelocity");

    void* pfn_DestroyManager = (void*)GetProcAddress(s_nativeCarXModule, "CX_DestroyManager");

    if (pfn_CreateCar) {
        HookManager::CreateHook(
            (void*)pfn_CreateCar,
            (void*)Hooked_CX_CreateCar,
            (void**)&s_originalCreateCar,
            "CX_CreateCar"
        );
    }

    if (pfn_DestroyCar) {
        HookManager::CreateHook(
            (void*)pfn_DestroyCar,
            (void*)Hooked_CX_DestroyCar,
            (void**)&s_originalDestroyCar,
            "CX_DestroyCar"
        );
    }

    if (pfn_DestroyManager) {
        HookManager::CreateHook(
            pfn_DestroyManager,
            (void*)Hooked_CX_DestroyManager,
            (void**)&s_originalDestroyManager,
            "CX_DestroyManager"
        );
    }

    void* pfn_SetRenderMatrix = (void*)GetProcAddress(s_nativeCarXModule, "CX_CarSetRenderMatrix");
    if (pfn_SetRenderMatrix) {
        HookManager::CreateHook(
            pfn_SetRenderMatrix,
            (void*)Hooked_CX_CarSetRenderMatrix,
            (void**)&s_originalSetRenderMatrix,
            "CX_CarSetRenderMatrix"
        );
    } else {
        Logger::Error("Failed to find CX_CarSetRenderMatrix export in NativeCarX.dll!");
    }

    s_initialized = true;
    Logger::Info("NativeCarX module hooked and initialized.");
    return true;
}

void NativeCarX::Shutdown() {
    if (!s_initialized) return;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_trackedCars.clear();
    s_playerCarPtr = nullptr;
    s_initialized = false;
    Logger::Info("NativeCarX module shut down.");
}

void* NativeCarX::Hooked_CX_CreateCar(void* manager) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        return s_originalCreateCar ? s_originalCreateCar(manager) : nullptr;
    }
    if (manager) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_managerPtr = manager;
    }
    void* result = nullptr;
    if (s_originalCreateCar) {
        result = s_originalCreateCar(manager);
    }
    Logger::Info("NativeCarX CX_CreateCar called. Manager: " + std::to_string((uintptr_t)manager));
    return result;
}

void NativeCarX::Hooked_CX_DestroyCar(void* manager, void* car) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        if (s_originalDestroyCar) s_originalDestroyCar(manager, car);
        return;
    }
    t_CX_DestroyCar orig = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (car) {
            s_trackedCars.erase(
                std::remove_if(s_trackedCars.begin(), s_trackedCars.end(),
                    [car](const CarTelemetry& c) { return c.carPtr == car; }),
                s_trackedCars.end()
            );
            if (s_playerCarPtr == car) {
                s_playerCarPtr = nullptr;
            }
        }
        orig = s_originalDestroyCar;
        if (orig) {
            // Keep s_mutex locked across native free so render-thread telemetry cannot UAF `car`
            orig(manager, car);
            return;
        }
    }
}

void NativeCarX::Hooked_CX_DestroyManager(void* manager) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        if (s_originalDestroyManager) s_originalDestroyManager(manager);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_trackedCars.clear();
    s_playerCarPtr = nullptr;
    if (s_managerPtr == manager) {
        s_managerPtr = nullptr;
    }
    if (s_originalDestroyManager) {
        s_originalDestroyManager(manager);
    }
}

bool NativeCarX::IsCarMemoryValid(void* car) {
    uintptr_t p = reinterpret_cast<uintptr_t>(car);
    if (p < 0x10000) return false;
    const DWORD rwMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    // Verify both [car + 0x1370D8] (CX_CarGetSpeedKMH) and [car + 0x137380] (list links) are committed/readable
    uintptr_t cur = p + 0x1370D8;
    const uintptr_t end = p + 0x137388;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if ((mbi.Protect & rwMask) == 0) return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur) return false;
        cur = regionEnd;
    }
    return true;
}

void NativeCarX::PruneStaleCarsLocked() {
    auto now = std::chrono::steady_clock::now();
    s_trackedCars.erase(
        std::remove_if(s_trackedCars.begin(), s_trackedCars.end(),
            [now](const CarTelemetry& c) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.lastUpdateTime).count();
                return ms > 1500 || !IsCarMemoryValid(c.carPtr);
            }),
        s_trackedCars.end()
    );
    if (s_playerCarPtr) {
        bool stillPresent = std::any_of(s_trackedCars.begin(), s_trackedCars.end(),
            [](const CarTelemetry& c) { return c.carPtr == s_playerCarPtr; });
        if (!stillPresent) {
            s_playerCarPtr = nullptr;
        }
    }
}

void NativeCarX::Hooked_CX_CarSetRenderMatrix(void* car, const float* matrix, float factor) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        if (s_originalSetRenderMatrix) s_originalSetRenderMatrix(car, matrix, factor);
        return;
    }
    static int s_loggedReturnAddrs = 0;
    if (s_loggedReturnAddrs < 10) {
        void* retAddr = _ReturnAddress();
        HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
        uintptr_t rva = hGA ? ((uintptr_t)retAddr - (uintptr_t)hGA) : (uintptr_t)retAddr;
        char buf[64];
        sprintf_s(buf, "0x%llX", (unsigned long long)rva);
        Logger::Info(std::string("CX_CarSetRenderMatrix caller Return RVA: ") + buf + ", car=" + std::to_string((uintptr_t)car));
        s_loggedReturnAddrs++;
    }
    // 1. Forward immediately to original game function so xmm2 (factor) is 100% clean and intact
    if (s_originalSetRenderMatrix) {
        s_originalSetRenderMatrix(car, matrix, factor);
    }

    // 2. Track car and record matrix
    if (matrix && car) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        auto now = std::chrono::steady_clock::now();

        // Remove dead/inactive cars
        PruneStaleCarsLocked();

        // Find or create car entry
        auto it = std::find_if(s_trackedCars.begin(), s_trackedCars.end(),
            [car](const CarTelemetry& c) { return c.carPtr == car; });

        if (it == s_trackedCars.end()) {
            CarTelemetry newCar;
            newCar.carPtr = car;
            s_trackedCars.push_back(newCar);
            it = s_trackedCars.end() - 1;
        }

        it->lastUpdateTime = now;
        for (int i = 0; i < 16; ++i) {
            it->transformMatrix[i] = matrix[i];
        }
        it->posX = matrix[12];
        it->posY = matrix[13];
        it->posZ = matrix[14];

        // If local player car pointer not set yet, set it to the first active car
        if (!s_playerCarPtr) {
            s_playerCarPtr = car;
            it->isLocalPlayer = true;
        } else if (s_playerCarPtr == car) {
            it->isLocalPlayer = true;
        } else {
            it->isLocalPlayer = false;
        }
    }
}

void NativeCarX::UpdateCarTelemetry(CarTelemetry& tele) {
    void* car = tele.carPtr;
    if (!IsCarMemoryValid(car)) return;

    if (pfn_GetSpeedKMH) tele.speedKmh = pfn_GetSpeedKMH(car);
    if (pfn_GetRPM) tele.rpm = pfn_GetRPM(car);
    if (pfn_GetGear) tele.gear = pfn_GetGear(car);
    if (pfn_GetSteerAngle) tele.steerAngle = pfn_GetSteerAngle(car);
    if (pfn_GetAccel) tele.accel = pfn_GetAccel(car);
    if (pfn_GetBrake) tele.brake = pfn_GetBrake(car);
    if (pfn_GetHandBrake) tele.handbrake = pfn_GetHandBrake(car);
    if (pfn_GetClutch) tele.clutch = pfn_GetClutch(car);

    if (pfn_GetWheel) {
        for (int i = 0; i < 4; ++i) {
            void* wheel = pfn_GetWheel(car, i);
            if (!wheel) continue;

            if (pfn_GetWheelMatrix) {
                pfn_GetWheelMatrix(wheel, tele.wheels[i].matrix.data());
            }
            if (pfn_GetCaliperMatrix) {
                pfn_GetCaliperMatrix(wheel, tele.wheels[i].caliperMatrix.data());
            }
            if (pfn_GetSmoke) {
                tele.wheels[i].smoke = pfn_GetSmoke(wheel);
            }
            if (pfn_GetSmokeIntensity) {
                tele.wheels[i].smokeIntensity = pfn_GetSmokeIntensity(wheel);
            }
            if (pfn_GetCamber) {
                tele.wheels[i].camber = pfn_GetCamber(wheel);
            }
            if (pfn_GetToeIn) {
                tele.wheels[i].toeIn = pfn_GetToeIn(wheel);
            }
        }
    }
}

CarTelemetry NativeCarX::GetPlayerCarTelemetry() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    PruneStaleCarsLocked();
    if (s_trackedCars.empty()) {
        return CarTelemetry();
    }

    // 1. Look for explicit player car pointer
    if (s_playerCarPtr) {
        for (auto& c : s_trackedCars) {
            if (c.carPtr == s_playerCarPtr) {
                UpdateCarTelemetry(c);
                return c;
            }
        }
    }

    // 2. Look for any car receiving user inputs
    for (auto& c : s_trackedCars) {
        UpdateCarTelemetry(c);
        if (c.accel > 0.05f || c.brake > 0.05f || c.handbrake > 0.05f || abs(c.steerAngle) > 0.05f) {
            s_playerCarPtr = c.carPtr;
            c.isLocalPlayer = true;
            return c;
        }
    }

    // 3. Fallback: return the first active car in session
    s_playerCarPtr = s_trackedCars.front().carPtr;
    s_trackedCars.front().isLocalPlayer = true;
    UpdateCarTelemetry(s_trackedCars.front());
    return s_trackedCars.front();
}

std::vector<CarTelemetry> NativeCarX::GetAllCarsTelemetry() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    PruneStaleCarsLocked();
    for (auto& c : s_trackedCars) {
        UpdateCarTelemetry(c);
    }
    return s_trackedCars;
}

bool NativeCarX::IsInSession() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    PruneStaleCarsLocked();
    return !s_trackedCars.empty();
}

void NativeCarX::SetCarRenderMatrix(void* car, const float* matrix16, float factor) {
    if (s_originalSetRenderMatrix && car && matrix16) {
        s_originalSetRenderMatrix(car, matrix16, factor);
    }
}

void* NativeCarX::GetPlayerCarPtr() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_playerCarPtr;
}

void NativeCarX::SetCarActive(void* car, bool active) {
    if (pfn_CarSetActive && car) {
        pfn_CarSetActive(car, active ? 1 : 0);
    }
}

void NativeCarX::SetCarRPM(void* car, float rpm) {
    if (pfn_CarSetRPM && car) {
        pfn_CarSetRPM(car, rpm);
    }
}

void NativeCarX::SetCarGear(void* car, int gear) {
    if (pfn_CarSetGear && car) {
        pfn_CarSetGear(car, gear);
    }
}

bool NativeCarX::GetCarRigidBody(void* car, float outRb[29]) {
    if (!pfn_GetRigidBody || !car || !outRb) return false;
    memset(outRb, 0, sizeof(float) * 29);
    pfn_GetRigidBody(car, outRb);
    return true;
}

bool NativeCarX::SetCarRigidBody(void* car, const float inRb[29]) {
    if (!pfn_SetRigidBody || !car || !inRb) return false;
    pfn_SetRigidBody(car, inRb);
    return true;
}

bool NativeCarX::GetCarVelocity(void* car, float outVel[3]) {
    if (!pfn_GetVelocity || !car || !outVel) return false;
    pfn_GetVelocity(car, outVel);
    return true;
}

void* NativeCarX::SpawnCar() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_managerPtr || !pfn_CreateCar) {
        Logger::Warn("Cannot spawn car: Manager pointer not yet captured.");
        return nullptr;
    }
    void* car = pfn_CreateCar(s_managerPtr);
    if (car) {
        if (s_playerCarPtr && pfn_GetRigidBody && pfn_SetRigidBody) {
            char rb[128] = {0};
            pfn_GetRigidBody(s_playerCarPtr, rb);
            // Zero out velocities so ghost is completely stationary
            memset(rb + 0x40, 0, 24); // linear velocity
            memset(rb + 0x50, 0, 24); // angular velocity
            pfn_SetRigidBody(car, rb);
        }

        if (pfn_CarSetActive) {
            pfn_CarSetActive(car, 0); // Disable physical simulation on ghost
        }
        Logger::Info("Spawned AFK Ghost Car at: " + std::to_string((uintptr_t)car));
    }
    return car;
}

bool NativeCarX::DestroyCar(void* car) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_managerPtr || !pfn_DestroyCar || !car) return false;
    pfn_DestroyCar(s_managerPtr, car);
    Logger::Info("Destroyed Ghost Car at: " + std::to_string((uintptr_t)car));
    return true;
}

void* NativeCarX::GetManagerPtr() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_managerPtr;
}
