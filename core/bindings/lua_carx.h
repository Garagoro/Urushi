#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaCarX {
public:
    static void Register(lua_State* L);

    static int GetPlayerCar(lua_State* L);
    static int GetAllCars(lua_State* L);
    static int GetPlayerList(lua_State* L);
    static int IsInSession(lua_State* L);
    static int SetCarRenderMatrix(lua_State* L);
    static int SetCarActive(lua_State* L);
    static int SetCarRPM(lua_State* L);
    static int SetCarGear(lua_State* L);
    static int SpawnCar(lua_State* L);
    static int DestroyCar(lua_State* L);
    static int SpawnGhost(lua_State* L);
    static int DestroyGhost(lua_State* L);
    static int HasGhost(lua_State* L);
    static int GetGhost(lua_State* L);
    static int SetGhostMirror(lua_State* L);
    static int IsGhostMirror(lua_State* L);
    static int SetGhostCollision(lua_State* L);
    static int IsGhostCollision(lua_State* L);
    static int GetGhostCount(lua_State* L);
    static int SetSelectiveGhostConfig(lua_State* L);
    static int GetSelectiveGhostConfig(lua_State* L);
    static int SetCarGhostMode(lua_State* L);
    static int ClearCarGhostModes(lua_State* L);
    static int GetRigidBody(lua_State* L);
    static int TeleportRigidBody(lua_State* L);
    static int GetTime(lua_State* L);
    static int GetDeltaTime(lua_State* L);

    // Dynamic Physics & World API
    static int GetVelocity(lua_State* L);
    static int SetVelocity(lua_State* L);
    static int GetAngularVelocity(lua_State* L);
    static int SetAngularVelocity(lua_State* L);
    static int GetDriftInfo(lua_State* L);
    static int GetCamera(lua_State* L);
    static int Raycast(lua_State* L);
    static int ResetCar(lua_State* L);
    static int GetProximity(lua_State* L);

    // Damage & Tire Wear API
    static int SetNoDamage(lua_State* L);
    static int GetNoDamage(lua_State* L);
    static int SetNoTireWear(lua_State* L);
    static int GetNoTireWear(lua_State* L);
    static int RepairCar(lua_State* L);
    static int ResetTireWear(lua_State* L);
};
