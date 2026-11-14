#include "unity_bridge.h"
#include "unity_bridge_internal.h"
#include "pattern_scanner.h"
#include "ecs_collision.h"
#include "native_carx.h"
#include "logger.h"
#include <cmath>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace UnityBridgeInternal;

namespace UnityBridgeInternal {

void* GetGameObjectManagerPtr(uintptr_t unityBase) {
    static void* s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindRipOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "B8 FF FF FF FF 48 8B 0D ? ? ? ? 8B D0", 7, 3, 0x2232770, 5, "GameObjectManager");
        s_cached = reinterpret_cast<void*>(va);
    }
    return s_cached;
}

void* GetGCHandleTargetPtr(uintptr_t unityBase) {
    static void* s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindRipOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "C3 48 8B 05 ? ? ? ? 48 8B 49 18 FF D0", 7, 3, 0x22339F0, 1, "GCHandleTarget");
        s_cached = reinterpret_cast<void*>(va);
    }
    return s_cached;
}

uintptr_t SafeFindCarBehaviourOnRootGoSEH(void* rootGo) {
    if (!rootGo) return 0;
    __try {
        uintptr_t go = reinterpret_cast<uintptr_t>(rootGo);
        if (!IsReadableMem(go, 0x38)) return 0;
        uintptr_t compArr = *reinterpret_cast<uintptr_t*>(go + 0x20);
        uint32_t compCount = *reinterpret_cast<uint32_t*>(go + 0x30);
        if (compCount == 0 || compCount > 256 || !IsReadableMem(compArr, static_cast<size_t>(compCount) * 16)) return 0;

        for (uint32_t ci = 0; ci < compCount; ++ci) {
            uint32_t cid = *reinterpret_cast<uint32_t*>(compArr + ci * 16);
            if (cid != 66) continue; // MonoBehaviour
            uintptr_t natComp = *reinterpret_cast<uintptr_t*>(compArr + ci * 16 + 8);
            if (!IsReadableMem(natComp, 0x20)) continue;
            uintptr_t gcSlot = *reinterpret_cast<uintptr_t*>(natComp + 0x18);
            if (!IsReadableMem(gcSlot & ~7ull, sizeof(uintptr_t))) continue;
            uintptr_t managed = *reinterpret_cast<uintptr_t*>(gcSlot & ~7ull);
            if ((managed & 7) || !IsReadableMem(managed, 0x118)) continue;
            uintptr_t klass = *reinterpret_cast<uintptr_t*>(managed);
            if (!IsReadableMem(klass, 0x18)) continue;
            const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
            if (!IsReadableMem(reinterpret_cast<uintptr_t>(kname), 16)) continue;
            if (kname[0] == 'C' && kname[1] == 'a' && kname[2] == 'r' &&
                strcmp(kname, "CarBehaviour") == 0) {
                return managed;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

bool SafeInspectCarBehaviourOnRootGoSEH(void* rootGo, CarBehaviourInfo* outInfo) {
    if (!rootGo || !outInfo) return false;
    *outInfo = CarBehaviourInfo{};
    uintptr_t cb = SafeFindCarBehaviourOnRootGoSEH(rootGo);
    if (cb < 0x10000) return false;

    __try {
        outInfo->carBehaviour = cb;
        // Verified offsets in CarBehaviour [idx=23722]:
        //   +0x50: <carX>k__BackingField (CARXCar : Car) -> +0xC8: Car.native (NativeCarX handle)
        //   +0x58: Framework.Models.Cars.RaceCars.RaceCar* raceCarModel
        //   +0x110: uint8_t m_isPlayer
        outInfo->isPlayer = (*reinterpret_cast<const uint8_t*>(cb + 0x110) != 0);

        uintptr_t carXObj = *reinterpret_cast<const uintptr_t*>(cb + 0x50);
        if (IsReadableMem(carXObj, 0xD0)) {
            void* nativePtr = *reinterpret_cast<void* const*>(carXObj + 0xC8);
            if (reinterpret_cast<uintptr_t>(nativePtr) >= 0x10000) {
                outInfo->nativeCarPtr = nativePtr;
            }
        }

        uintptr_t raceCar = *reinterpret_cast<const uintptr_t*>(cb + 0x58);
        if (IsReadableMem(raceCar, 0x160)) {
            outInfo->raceCarPtr = reinterpret_cast<void*>(raceCar);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outInfo = CarBehaviourInfo{};
        return false;
    }
}

} // namespace UnityBridgeInternal

// ================= Native / Managed Helpers =================

void* UnityBridge::UnwrapScriptingPtrToNative(void* gcOrObj) {
    uintptr_t raw = reinterpret_cast<uintptr_t>(gcOrObj);
    if (!raw || !s_unityBase) return nullptr;

    void* vtbl = nullptr;
    if (SafeReadPtr(gcOrObj, &vtbl)) {
        uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vtbl);
        if (vtAddr >= s_unityBase && vtAddr < s_unityBase + s_unitySize) {
            return gcOrObj;
        }
    }

    void* csObj = nullptr;
    if ((raw & 1) == 0) {
        void* maybeCached = nullptr;
        if (SafeReadPtr(reinterpret_cast<void*>(raw + 0x10), &maybeCached) && maybeCached) {
            void* nativeVt = nullptr;
            if (SafeReadPtr(maybeCached, &nativeVt)) {
                uintptr_t nvt = reinterpret_cast<uintptr_t>(nativeVt);
                if (nvt >= s_unityBase && nvt < s_unityBase + s_unitySize) {
                    return maybeCached;
                }
            }
        }
        SafeReadPtr(gcOrObj, &csObj);
    } else {
        void* pfnTarget = nullptr;
        void* targetSlot = GetGCHandleTargetPtr(s_unityBase);
        if (targetSlot && SafeReadPtr(targetSlot, &pfnTarget) && pfnTarget) {
            csObj = SafeCallGCHandleTarget(pfnTarget, raw);
        }
    }

    if (csObj) {
        void* nativePtr = nullptr;
        if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(csObj) + 0x10), &nativePtr)) {
            return nativePtr;
        }
    }

    return nullptr;
}

void* UnityBridge::GetTransformOfGameObject(void* nativeGo) {
    if (!nativeGo) return nullptr;
    void* compArray = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeGo) + 0x20), &compArray) || !compArray) {
        return nullptr;
    }
    void* tf = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(compArray) + 0x08), &tf)) {
        return tf;
    }
    return nullptr;
}

void* UnityBridge::GetGameObjectOfTransform(void* nativeTf) {
    if (!nativeTf) return nullptr;
    void* go = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeTf) + 0x20), &go)) {
        return go;
    }
    return nullptr;
}

std::string UnityBridge::GetGameObjectName(void* nativeGo) {
    if (!nativeGo) return "";
    void* namePtr = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeGo) + 0x50), &namePtr) || !namePtr) {
        return "";
    }
    char buf[128] = { 0 };
    if (SafeReadCStr(namePtr, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "";
}

bool UnityBridge::GetTransformHierarchyPosition(void* nativeTf, UnityVector3& outPos) {
    if (!nativeTf) return false;
    void* hier = nullptr;
    uint32_t idx = 0;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeTf) + 0x28), &hier) || !hier) {
        return false;
    }
    if (!SafeReadU32(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeTf) + 0x30), &idx)) {
        return false;
    }
    if (idx > 65536) return false;

    void* localTransforms = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hier) + 0x18), &localTransforms) || !localTransforms) {
        return false;
    }

    uintptr_t elemAddr = reinterpret_cast<uintptr_t>(localTransforms) + static_cast<size_t>(idx) * 48;
    float xyz[3] = { 0 };
    if (SafeReadBytes(reinterpret_cast<void*>(elemAddr), xyz, sizeof(xyz))) {
        if (std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2])) {
            outPos.x = xyz[0];
            outPos.y = xyz[1];
            outPos.z = xyz[2];
            return true;
        }
    }
    return false;
}

// ================= Scene Enumeration & Car Visual Cache =================

std::vector<UnityObjectEntry> UnityBridge::GetAllGameObjects(int maxObjects) {
    std::vector<UnityObjectEntry> result;
    if (!s_unityBase) return result;

    void* mgr = nullptr;
    void* mgrSlot = GetGameObjectManagerPtr(s_unityBase);
    if (!mgrSlot || !SafeReadPtr(mgrSlot, &mgr) || !mgr) {
        return result;
    }

    std::unordered_set<void*> seen;
    seen.reserve(4096);
    result.reserve(4096);

    auto addGo = [&](void* go) {
        if (!go || reinterpret_cast<uintptr_t>(go) < 0x10000) return;
        if (!seen.insert(go).second) return;
        if (static_cast<int>(result.size()) >= maxObjects) return;

        UnityObjectEntry entry;
        entry.go = go;
        entry.transform = GetTransformOfGameObject(go);
        if (entry.transform) {
            SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry.transform) + 0x68), &entry.parentTransform);
            entry.hasPos = GetTransformHierarchyPosition(entry.transform, entry.pos);
        }
        entry.name = GetGameObjectName(go);
        result.push_back(std::move(entry));
    };

    uintptr_t sentinel = reinterpret_cast<uintptr_t>(mgr) + 0x18;
    uintptr_t node = 0;
    if (SafeReadU64(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgr) + 0x20), &node)) {
        int guard = 0;
        while (node && node != sentinel && guard++ < maxObjects) {
            void* go = nullptr;
            if (SafeReadPtr(reinterpret_cast<void*>(node + 0x10), &go)) {
                addGo(go);
            }
            uintptr_t nextNode = 0;
            if (!SafeReadU64(reinterpret_cast<void*>(node + 0x08), &nextNode)) break;
            node = nextNode;
        }
    }

    uintptr_t entries = 0;
    uint32_t bucketCountMinus8 = 0;
    if (SafeReadU64(mgr, &entries) && entries &&
        SafeReadU32(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgr) + 0x08), &bucketCountMinus8)) {
        uint32_t buckets = bucketCountMinus8 + 8;
        if (buckets > 8192) buckets = 8192;

        for (uint32_t i = 0; i < buckets && static_cast<int>(result.size()) < maxObjects; ++i) {
            uintptr_t bucket = entries + static_cast<size_t>(i) * 24;
            uint32_t flag = 0xFFFFFFFF;
            if (!SafeReadU32(reinterpret_cast<void*>(bucket), &flag) || flag >= 0xFFFFFFFE) {
                continue;
            }
            uintptr_t listSentinel = 0;
            if (!SafeReadU64(reinterpret_cast<void*>(bucket + 0x10), &listSentinel) || listSentinel < 0x10000) {
                continue;
            }
            uintptr_t n = 0;
            if (!SafeReadU64(reinterpret_cast<void*>(listSentinel + 0x08), &n)) {
                continue;
            }
            int guard = 0;
            while (n && n != listSentinel && guard++ < maxObjects && static_cast<int>(result.size()) < maxObjects) {
                void* go = nullptr;
                if (SafeReadPtr(reinterpret_cast<void*>(n + 0x10), &go)) {
                    addGo(go);
                }
                uintptr_t nextN = 0;
                if (!SafeReadU64(reinterpret_cast<void*>(n + 0x08), &nextN)) break;
                n = nextN;
            }
        }
    }

    return result;
}

void* UnityBridge::FindGameObjectByName(const char* name) {
    if (!pfn_Find || !name) return nullptr;
    void* tf = SafeCallFind(reinterpret_cast<void*>(pfn_Find), name);
    if (!tf) return nullptr;
    return GetGameObjectOfTransform(tf);
}

static bool SafeGetSteamPersonaNameSEH(void* pfnFriends, void* pfnGetName, char* outBuf, size_t outSize) {
    if (!pfnFriends || !pfnGetName || !outBuf || outSize == 0) return false;
    __try {
        typedef void* (*t_Friends)();
        typedef const char* (*t_GetName)(void*);
        void* friends = reinterpret_cast<t_Friends>(pfnFriends)();
        if (friends) {
            const char* name = reinterpret_cast<t_GetName>(pfnGetName)(friends);
            if (name && name[0] != '\0') {
                size_t i = 0;
                for (; i + 1 < outSize && name[i] != '\0'; ++i) {
                    outBuf[i] = name[i];
                }
                outBuf[i] = '\0';
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static std::string GetLocalSteamPersonaName() {
    static std::string s_cachedName;
    static bool s_checked = false;
    if (s_checked) return s_cachedName;
    s_checked = true;
    HMODULE hSteam = GetModuleHandleW(L"steam_api64.dll");
    if (!hSteam) {
        hSteam = LoadLibraryW(L"steam_api64.dll");
    }
    if (hSteam) {
        void* pfnFriends = reinterpret_cast<void*>(GetProcAddress(hSteam, "SteamAPI_SteamFriends_v017"));
        void* pfnGetName = reinterpret_cast<void*>(GetProcAddress(hSteam, "SteamAPI_ISteamFriends_GetPersonaName"));
        char buf[128] = { 0 };
        if (SafeGetSteamPersonaNameSEH(pfnFriends, pfnGetName, buf, sizeof(buf))) {
            s_cachedName = buf;
        }
    }
    return s_cachedName;
}

static std::string StripRichTextTags(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    bool insideTag = false;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '<') {
            insideTag = true;
        } else if (str[i] == '>') {
            insideTag = false;
        } else if (!insideTag) {
            result += str[i];
        }
    }
    size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, (last - first + 1));
}

static std::string Utf16ToUtf8(const wchar_t* wstr, int len) {
    if (!wstr || len <= 0) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, len, NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return "";
    std::string out(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &out[0], sizeNeeded, NULL, NULL);
    return out;
}

static bool SafeGetIl2CppStringCharsSEH(void* il2cppStringPtr, const wchar_t** outChars, int32_t* outLen) {
    if (!il2cppStringPtr || !outChars || !outLen) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(il2cppStringPtr);
    if (!IsReadableMem(addr, 0x18)) return false;
    __try {
        int32_t len = *reinterpret_cast<const int32_t*>(addr + 0x10);
        if (len <= 0 || len > 256) return false;
        const wchar_t* chars = reinterpret_cast<const wchar_t*>(addr + 0x14);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(chars), static_cast<size_t>(len) * sizeof(wchar_t))) return false;
        *outChars = chars;
        *outLen = len;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::string ReadIl2CppString(void* il2cppStringPtr) {
    if (!il2cppStringPtr) return "";
    const wchar_t* chars = nullptr;
    int32_t len = 0;
    if (SafeGetIl2CppStringCharsSEH(il2cppStringPtr, &chars, &len) && chars && len > 0) {
        return Utf16ToUtf8(chars, len);
    }
    return "";
}

static uintptr_t SafeFindNickNamesContextSEH(void* nickContextGo) {
    if (!nickContextGo) return 0;
    __try {
        uintptr_t go = reinterpret_cast<uintptr_t>(nickContextGo);
        if (!IsReadableMem(go, 0x38)) return 0;
        uintptr_t compArr = *reinterpret_cast<uintptr_t*>(go + 0x20);
        uint32_t compCount = *reinterpret_cast<uint32_t*>(go + 0x30);
        if (compCount == 0 || compCount > 256 || !IsReadableMem(compArr, static_cast<size_t>(compCount) * 16)) return 0;
        for (uint32_t ci = 0; ci < compCount; ++ci) {
            uint32_t cid = *reinterpret_cast<uint32_t*>(compArr + ci * 16);
            if (cid != 66) continue;
            uintptr_t natComp = *reinterpret_cast<uintptr_t*>(compArr + ci * 16 + 8);
            if (!IsReadableMem(natComp, 0x20)) continue;
            uintptr_t gcSlot = *reinterpret_cast<uintptr_t*>(natComp + 0x18);
            if (!IsReadableMem(gcSlot & ~7ull, sizeof(uintptr_t))) continue;
            uintptr_t managed = *reinterpret_cast<uintptr_t*>(gcSlot & ~7ull);
            if ((managed & 7) || !IsReadableMem(managed, 0x70)) continue;
            uintptr_t klass = *reinterpret_cast<uintptr_t*>(managed);
            if (!IsReadableMem(klass, 0x18)) continue;
            const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
            if (kname && IsReadableMem(reinterpret_cast<uintptr_t>(kname), 30) &&
                strcmp(kname, "UIPlayersNickNamesContext") == 0) {
                return managed;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static bool SafeGetNickDictCountAndEntriesSEH(uintptr_t context, uint32_t* outCount, uintptr_t* outEntries) {
    if (!context || !outCount || !outEntries) return false;
    __try {
        if (!IsReadableMem(context, 0x70)) return false;
        uintptr_t dict = *reinterpret_cast<const uintptr_t*>(context + 0x60);
        if (!IsReadableMem(dict, 0x30)) return false;
        uint32_t count = *reinterpret_cast<const uint32_t*>(dict + 0x20);
        uintptr_t entries = *reinterpret_cast<const uintptr_t*>(dict + 0x18);
        if (!entries || count == 0 || count > 128) return false;
        if (!IsReadableMem(entries, 0x20 + count * 24)) return false;
        *outCount = count;
        *outEntries = entries;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadSingleNickDataSEH(uintptr_t entryBase, void** outNativeTf, void** outIl2CppString, float outPos[3]) {
    if (!outNativeTf || !outIl2CppString || !outPos) return false;
    __try {
        uintptr_t val = *reinterpret_cast<const uintptr_t*>(entryBase + 16);
        if (!IsReadableMem(val, 0x70)) return false;
        uintptr_t carTfManaged = *reinterpret_cast<const uintptr_t*>(val + 0x38);
        void* nativeTf = nullptr;
        if (carTfManaged && IsReadableMem(carTfManaged, 0x20)) {
            nativeTf = *reinterpret_cast<void* const*>(carTfManaged + 0x10);
        }
        *outNativeTf = nativeTf;
        *outIl2CppString = *reinterpret_cast<void* const*>(val + 0x50);
        outPos[0] = *reinterpret_cast<const float*>(val + 0x40);
        outPos[1] = *reinterpret_cast<const float*>(val + 0x44);
        outPos[2] = *reinterpret_cast<const float*>(val + 0x48);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UnityBridge::RefreshLiveCarListOnMainThread(bool forceRescan) {
    if (!IsOnMainThread()) return;

    auto cars = NativeCarX::GetAllCarsTelemetry();
    auto now = std::chrono::steady_clock::now();
    float sinceScan = std::chrono::duration<float>(now - s_lastSceneCarScan).count();

    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    bool needRescan = forceRescan || s_sceneCarCache.empty() || (sinceScan > 1.0f);
    if (!needRescan && cars.size() != s_liveCars.size() && sinceScan > 0.25f) {
        needRescan = true;
    }

    static uintptr_t s_cachedNickContext = 0;

    if (needRescan) {
        s_lastSceneCarScan = now;
        s_cachedNickContext = 0;
        auto objects = GetAllGameObjects(30000);
        std::unordered_map<void*, const UnityObjectEntry*> tfMap;
        tfMap.reserve(objects.size());
        void* nickContextGo = nullptr;
        for (const auto& obj : objects) {
            if (obj.transform) tfMap[obj.transform] = &obj;
            if (!nickContextGo && obj.name == "UIPlayersNickNames(Clone)") {
                nickContextGo = obj.go;
            }
        }

        if (nickContextGo) {
            s_cachedNickContext = SafeFindNickNamesContextSEH(nickContextGo);
        }

        std::unordered_set<void*> ownedClones;
        for (const auto& slot : s_ghostSlots) {
            if (slot.gameObject) ownedClones.insert(slot.gameObject);
        }

        std::vector<SceneCarModelCache> rebuilt;
        for (const auto& obj : objects) {
            if (obj.name != "Model" || !obj.parentTransform) continue;
            if (ownedClones.count(obj.go)) continue;

            auto it = tfMap.find(obj.parentTransform);
            if (it == tfMap.end()) continue;
            const UnityObjectEntry* root = it->second;
            if (root->parentTransform != nullptr) continue; // real cars are top-level scene roots

            SceneCarModelCache item;
            item.unityName = root->name;
            item.rootGo = root->go;
            item.rootTf = root->transform;
            item.modelGo = obj.go;

            CarBehaviourInfo cbInfo{};
            if (SafeInspectCarBehaviourOnRootGoSEH(item.rootGo, &cbInfo)) {
                item.carBehaviour = reinterpret_cast<void*>(cbInfo.carBehaviour);
                item.nativeCarPtr = cbInfo.nativeCarPtr;
                item.raceCarPtr = cbInfo.raceCarPtr;
                item.isPlayer = cbInfo.isPlayer;
            }

            if (pfn_Transform_get_position && SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), item.rootTf, &item.pos)) {
                item.hasPos = true;
            } else if (root->hasPos) {
                item.pos = root->pos;
                item.hasPos = true;
            }
            if (pfn_Transform_get_rotation && item.rootTf) {
                item.hasRot = SafeCallGetVec4(reinterpret_cast<void*>(pfn_Transform_get_rotation), item.rootTf, &item.rot);
            }
            rebuilt.push_back(std::move(item));
        }
        s_sceneCarCache = std::move(rebuilt);
    } else if (pfn_Transform_get_position || pfn_Transform_get_rotation) {
        for (auto& sc : s_sceneCarCache) {
            if (sc.rootTf) {
                if (pfn_Transform_get_position) {
                    sc.hasPos = SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), sc.rootTf, &sc.pos);
                }
                if (pfn_Transform_get_rotation) {
                    sc.hasRot = SafeCallGetVec4(reinterpret_cast<void*>(pfn_Transform_get_rotation), sc.rootTf, &sc.rot);
                }
            }
            if (sc.rootGo && (!sc.carBehaviour || !sc.nativeCarPtr || !sc.raceCarPtr)) {
                CarBehaviourInfo cbInfo{};
                if (SafeInspectCarBehaviourOnRootGoSEH(sc.rootGo, &cbInfo)) {
                    sc.carBehaviour = reinterpret_cast<void*>(cbInfo.carBehaviour);
                    sc.nativeCarPtr = cbInfo.nativeCarPtr;
                    sc.raceCarPtr = cbInfo.raceCarPtr;
                    sc.isPlayer = cbInfo.isPlayer;
                }
            }
        }
    }

    // Extract multiplayer nicknames from UIPlayersNickNamesContext
    struct RemoteNickEntry {
        void* nativeTf = nullptr;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        std::string name;
    };
    std::vector<RemoteNickEntry> remoteNicks;

    uint32_t nickCount = 0;
    uintptr_t nickEntries = 0;
    if (SafeGetNickDictCountAndEntriesSEH(s_cachedNickContext, &nickCount, &nickEntries)) {
        for (uint32_t i = 0; i < nickCount; ++i) {
            uintptr_t entryBase = nickEntries + 0x20 + i * 24;
            void* nativeTf = nullptr;
            void* il2cppStr = nullptr;
            float pos[3] = { 0.0f, 0.0f, 0.0f };
            if (SafeReadSingleNickDataSEH(entryBase, &nativeTf, &il2cppStr, pos)) {
                std::string rawName = ReadIl2CppString(il2cppStr);
                std::string clean = StripRichTextTags(rawName);
                if (!clean.empty()) {
                    RemoteNickEntry rne;
                    rne.nativeTf = nativeTf;
                    rne.name = clean;
                    rne.x = pos[0];
                    rne.y = pos[1];
                    rne.z = pos[2];
                    remoteNicks.push_back(std::move(rne));
                }
            }
        }
    }

    std::string localNick = GetLocalSteamPersonaName();
    if (localNick.empty()) localNick = "Вы";

    for (auto& sc : s_sceneCarCache) {
        if (sc.isPlayer) {
            sc.playerName = localNick;
        } else {
            std::string matchedNick;
            if (sc.rootTf) {
                for (const auto& rne : remoteNicks) {
                    if (rne.nativeTf && rne.nativeTf == sc.rootTf) {
                        matchedNick = rne.name;
                        break;
                    }
                }
            }
            if (matchedNick.empty() && sc.hasPos) {
                float bestDistSq = 25.0f; // within 5m
                for (const auto& rne : remoteNicks) {
                    float dx = rne.x - sc.pos.x;
                    float dy = rne.y - sc.pos.y;
                    float dz = rne.z - sc.pos.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestDistSq) {
                        bestDistSq = d2;
                        matchedNick = rne.name;
                    }
                }
            }
            if (!matchedNick.empty()) {
                sc.playerName = matchedNick;
            }
        }
    }

    // Push authoritative local player car pointer from Unity CarBehaviour::m_isPlayer into NativeCarX
    void* knownPlayerCarPtr = nullptr;
    for (const auto& sc : s_sceneCarCache) {
        if (sc.isPlayer && sc.nativeCarPtr) {
            knownPlayerCarPtr = sc.nativeCarPtr;
            break;
        }
    }
    if (knownPlayerCarPtr) {
        NativeCarX::SetKnownPlayerCarPtr(knownPlayerCarPtr);
    }

    std::vector<LiveCarInfo> updated;
    updated.reserve(cars.size());
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto& c = cars[i];
        LiveCarInfo info;
        info.carPtr = c.carPtr;
        info.x = c.posX;
        info.y = c.posY;
        info.z = c.posZ;
        info.speedKmh = c.speedKmh;

        const SceneCarModelCache* best = nullptr;
        for (const auto& sc : s_sceneCarCache) {
            if (sc.nativeCarPtr && sc.nativeCarPtr == c.carPtr) {
                best = &sc;
                break;
            }
        }
        if (!best) {
            float bestDistSq = 16.0f; // fallback within 4m
            for (const auto& sc : s_sceneCarCache) {
                if (!sc.hasPos) continue;
                float dx = sc.pos.x - c.posX;
                float dy = sc.pos.y - c.posY;
                float dz = sc.pos.z - c.posZ;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDistSq) {
                    bestDistSq = d2;
                    best = &sc;
                }
            }
        }

        if (best) {
            info.unityName = best->unityName;
            info.playerName = best->playerName;
            info.rootGo = best->rootGo;
            info.rootTf = best->rootTf;
            info.modelGo = best->modelGo;
            info.carBehaviour = best->carBehaviour;
            info.raceCarPtr = best->raceCarPtr;
            if (best->hasPos) {
                info.x = best->pos.x; info.y = best->pos.y; info.z = best->pos.z;
                info.hasPos = true;
            }
            if (best->hasRot) {
                info.qx = best->rot.x; info.qy = best->rot.y; info.qz = best->rot.z; info.qw = best->rot.w;
                info.hasRot = true;
            }
            info.isLocalPlayer = best->isPlayer || (knownPlayerCarPtr ? (c.carPtr == knownPlayerCarPtr) : c.isLocalPlayer);
        } else {
            info.isLocalPlayer = knownPlayerCarPtr ? (c.carPtr == knownPlayerCarPtr) : c.isLocalPlayer;
            info.unityName = info.isLocalPlayer ? "Player Car" : ("Car #" + std::to_string(i + 1));
            info.playerName = info.isLocalPlayer ? localNick : "";
        }
        updated.push_back(std::move(info));
    }

    s_liveCars = std::move(updated);
}

std::vector<LiveCarInfo> UnityBridge::GetLiveCarList() {
    auto cars = NativeCarX::GetAllCarsTelemetry();
    auto ecsStates = EcsCollision::GetCarStates();
    std::string localNick = GetLocalSteamPersonaName();
    if (localNick.empty()) localNick = "Вы";
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    std::vector<LiveCarInfo> out;
    out.reserve(cars.size());
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto& c = cars[i];
        LiveCarInfo info;
        info.carPtr = c.carPtr;
        info.isLocalPlayer = c.isLocalPlayer;
        info.x = c.posX;
        info.y = c.posY;
        info.z = c.posZ;
        info.speedKmh = c.speedKmh;

        bool matched = false;
        for (const auto& cached : s_liveCars) {
            if (cached.carPtr == c.carPtr) {
                info.unityName = cached.unityName;
                info.playerName = cached.playerName;
                info.rootGo = cached.rootGo;
                info.rootTf = cached.rootTf;
                info.modelGo = cached.modelGo;
                info.carBehaviour = cached.carBehaviour;
                info.raceCarPtr = cached.raceCarPtr;
                info.hasPos = cached.hasPos;
                if (cached.hasPos) {
                    info.x = cached.x; info.y = cached.y; info.z = cached.z;
                }
                if (cached.hasRot) {
                    info.qx = cached.qx; info.qy = cached.qy; info.qz = cached.qz; info.qw = cached.qw;
                    info.hasRot = true;
                }
                info.isLocalPlayer = cached.isLocalPlayer || c.isLocalPlayer;
                matched = true;
                break;
            }
        }
        if (!matched) {
            info.unityName = c.isLocalPlayer ? "Player Car" : ("Car #" + std::to_string(i + 1));
            info.playerName = c.isLocalPlayer ? localNick : "";
        }

        const CarGhostState* gst = nullptr;
        for (const auto& st : ecsStates) {
            if (st.carPtr && st.carPtr == c.carPtr) {
                gst = &st;
                break;
            }
        }
        if (!gst) {
            for (const auto& st : ecsStates) {
                if (!st.unityName.empty() && st.unityName == info.unityName) {
                    gst = &st;
                    break;
                }
            }
        }
        if (gst) {
            info.isWrongWay = gst->isWrongWay;
            info.isSelectivelyGhosted = gst->isSelectivelyGhosted;
            info.isGameGhosted = gst->isGameGhosted;
            info.headingDot = gst->headingDot;
            info.distToPlayer = gst->distanceMeters;
        }
        info.ghostOverride = static_cast<int>(EcsCollision::GetCarOverride(info.unityName, reinterpret_cast<uintptr_t>(info.carPtr)));
        out.push_back(std::move(info));
    }
    return out;
}

std::string UnityBridge::LookupCarUnityName(void* carPtr) {
    if (!carPtr) return "";
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (const auto& c : s_liveCars) {
        if (c.carPtr == carPtr) return c.unityName;
    }
    for (const auto& sc : s_sceneCarCache) {
        if (sc.nativeCarPtr == carPtr) return sc.unityName;
    }
    return "";
}

std::string UnityBridge::LookupCarPlayerName(void* carPtr) {
    if (!carPtr) return "";
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (const auto& c : s_liveCars) {
        if (c.carPtr == carPtr && !c.playerName.empty()) return c.playerName;
    }
    for (const auto& sc : s_sceneCarCache) {
        if (sc.nativeCarPtr == carPtr && !sc.playerName.empty()) return sc.playerName;
    }
    return "";
}

void* UnityBridge::FindModelForCarName(const std::string& preferredCarName, void** outRootTf) {
    if (outRootTf) *outRootTf = nullptr;
    if (!preferredCarName.empty()) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (s_sceneCarCache.empty() && IsOnMainThread()) {
            RefreshLiveCarListOnMainThread(true);
        }
        for (const auto& sc : s_sceneCarCache) {
            if (sc.modelGo && sc.unityName == preferredCarName) {
                if (outRootTf) *outRootTf = sc.rootTf;
                return sc.modelGo;
            }
        }
        std::string wantedBase = StripCarSuffix(preferredCarName);
        for (const auto& sc : s_sceneCarCache) {
            if (sc.modelGo && StripCarSuffix(sc.unityName) == wantedBase) {
                if (outRootTf) *outRootTf = sc.rootTf;
                return sc.modelGo;
            }
        }
    }
    return FindPlayerCarModel(outRootTf);
}

void* UnityBridge::FindPlayerCarModel(void** outRootTf) {
    if (outRootTf) *outRootTf = nullptr;

    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer && c.modelGo) {
                if (outRootTf) *outRootTf = c.rootTf;
                return c.modelGo;
            }
        }
        for (const auto& sc : s_sceneCarCache) {
            if (sc.isPlayer && sc.modelGo) {
                if (outRootTf) *outRootTf = sc.rootTf;
                return sc.modelGo;
            }
        }
    }

    CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
    if (!tele.carPtr) return nullptr;

    auto objects = GetAllGameObjects(30000);
    std::unordered_map<void*, const UnityObjectEntry*> tfMap;
    tfMap.reserve(objects.size());
    for (const auto& obj : objects) {
        if (obj.transform) tfMap[obj.transform] = &obj;
    }

    std::unordered_set<void*> ownedClones;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& slot : s_ghostSlots) {
            if (slot.gameObject) ownedClones.insert(slot.gameObject);
        }
    }

    void* bestModelGo = nullptr;
    void* bestRootTf = nullptr;
    float bestDistSq = 9.0f;

    for (const auto& obj : objects) {
        if (obj.name != "Model" || !obj.parentTransform) continue;
        if (ownedClones.count(obj.go)) continue;

        auto it = tfMap.find(obj.parentTransform);
        if (it == tfMap.end()) continue;

        const UnityObjectEntry* parent = it->second;
        UnityVector3 ppos{};
        bool gotPos = false;
        if (IsOnMainThread() && pfn_Transform_get_position) {
            gotPos = SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), parent->transform, &ppos);
        }
        if (!gotPos && parent->hasPos) {
            ppos = parent->pos;
            gotPos = true;
        }
        if (!gotPos) continue;

        float dx = ppos.x - tele.posX;
        float dy = ppos.y - tele.posY;
        float dz = ppos.z - tele.posZ;
        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestModelGo = obj.go;
            bestRootTf = parent->transform;
        }
    }

    if (outRootTf && bestRootTf) {
        *outRootTf = bestRootTf;
    }
    return bestModelGo;
}

// ================= Direct Unity Calls =================

void* UnityBridge::CloneGameObjectDirect(void* nativeGo) {
    if (!pfn_CloneSingle || !nativeGo || !IsOnMainThread()) return nullptr;
    void* gcOrObj = SafeCallClone(reinterpret_cast<void*>(pfn_CloneSingle), nativeGo);
    if (!gcOrObj) return nullptr;
    return UnwrapScriptingPtrToNative(gcOrObj);
}

bool UnityBridge::DestroyObjectDirect(void* nativeObj, float delay) {
    if (!pfn_Destroy || !nativeObj || !IsOnMainThread()) return false;
    return SafeCallDestroy(reinterpret_cast<void*>(pfn_Destroy), nativeObj, delay);
}

bool UnityBridge::SetActiveDirect(void* nativeGo, bool active) {
    if (!pfn_SetActive || !nativeGo || !IsOnMainThread()) return false;
    return SafeCallSetActive(reinterpret_cast<void*>(pfn_SetActive), nativeGo, active);
}

bool UnityBridge::SetLayerDirect(void* nativeGo, int layer) {
    if (!pfn_SetLayer || !nativeGo || !IsOnMainThread()) return false;
    return SafeCallSetLayer(reinterpret_cast<void*>(pfn_SetLayer), nativeGo, layer);
}

bool UnityBridge::GetTransformPosRotDirect(void* nativeTf, UnityVector3& outPos, UnityVector4& outRot) {
    if (!nativeTf || !IsOnMainThread()) return false;
    bool okP = pfn_Transform_get_position && SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), nativeTf, &outPos);
    bool okR = pfn_Transform_get_rotation && SafeCallGetVec4(reinterpret_cast<void*>(pfn_Transform_get_rotation), nativeTf, &outRot);
    return okP && okR;
}

bool UnityBridge::SetTransformPositionDirect(void* nativeTf, const UnityVector3& pos) {
    if (!nativeTf || !IsOnMainThread() || !pfn_Transform_set_position) return false;
    return SafeCallSetVec3(reinterpret_cast<void*>(pfn_Transform_set_position), nativeTf, &pos);
}

bool UnityBridge::SetTransformRotationDirect(void* nativeTf, const UnityVector4& rot) {
    if (!nativeTf || !IsOnMainThread() || !pfn_Transform_set_rotation) return false;
    return SafeCallSetVec4(reinterpret_cast<void*>(pfn_Transform_set_rotation), nativeTf, &rot);
}

bool UnityBridge::SetTransformPosRotDirect(void* nativeTf, const UnityVector3& pos, const UnityVector4& rot) {
    return SetTransformPositionDirect(nativeTf, pos) && SetTransformRotationDirect(nativeTf, rot);
}

void* UnityBridge::GetPlayerCSharpCar() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_playerCSharpCar;
}

void* UnityBridge::GetPlayerNativeObj() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_playerNativeObj;
}


static bool SafeTriggerCarReapplyTuningSEH(void* carBehaviour) {
    if (!carBehaviour || reinterpret_cast<uintptr_t>(carBehaviour) < 0x10000) return false;
    __try {
        uintptr_t cb = reinterpret_cast<uintptr_t>(carBehaviour);
        if (!IsReadableMem(cb, 0x120)) return false;
        // CarBehaviour::m_needApplyTuning (offset 0x11C): forces CarVisualTuningController.LoadAndPaint()
        // on the next frame, cleanly reattaching all detached parts and resetting meshes.
        *reinterpret_cast<uint8_t*>(cb + 0x11C) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UnityBridge::RepairPlayerCarOnMainThread() {
    if (!IsOnMainThread()) return false;

    // 1. Reset vanilla NativeCarX deformation & suspension damage
    NativeCarX::ResetCarDamage();
    NativeCarX::ResetTireWear();

    // 2. Locate local player's CarBehaviour
    void* localCb = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer && c.carBehaviour) {
                localCb = c.carBehaviour;
                break;
            }
        }
        if (!localCb) {
            for (const auto& sc : s_sceneCarCache) {
                if (sc.isPlayer && sc.carBehaviour) {
                    localCb = sc.carBehaviour;
                    break;
                }
            }
        }
    }

    if (!localCb) {
        RefreshLiveCarListOnMainThread(true);
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer && c.carBehaviour) {
                localCb = c.carBehaviour;
                break;
            }
        }
    }

    if (!localCb) return false;

    // 3. Trigger CarBehaviour::m_needApplyTuning = 1 to reattach all detached parts and rebuild meshes
    if (SafeTriggerCarReapplyTuningSEH(localCb)) {
        Logger::Info("Player car repair triggered (m_needApplyTuning=1)");
        return true;
    }
    return false;
}
